#!/usr/bin/env python3
"""Isolate repeated GPU runs; retain phase logs even on a crash or timeout."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import time


def phase(message: str) -> None:
    print(message, flush=True)


def worker(args) -> int:
    import caustica
    import numpy as np

    phase(f"native={caustica.__file__} mode={args.mode} scene={args.scene}")
    retained = []
    with caustica.EngineApp.create(
        width=320, height=240, headless=True, vulkan=True,
        debug=args.debug, scene=args.scene, realtime=True,
    ) as engine:
        phase("created; waiting for scene")
        if not engine.wait_until_ready(timeout_seconds=120.0):
            raise RuntimeError("scene did not become ready")
        retained.append(engine.scene)
        if args.mode == "sensor":
            mask = int(caustica.Aov.rgb) | int(caustica.Aov.depth)
            for index in range(args.cameras):
                camera = engine.spawn_camera(
                    name=f"stress_{index}",
                    local_translation=(0.2 + index * 0.1, 1.0, 2.0),
                    aovs=mask,
                )
                if camera is None:
                    raise RuntimeError("spawn_camera failed")
                retained.append(camera)
        for index in range(args.captures):
            phase(f"iteration={index} step")
            if not engine.step_frame():
                raise RuntimeError("step_frame failed")
            if args.mode == "readback":
                phase(f"iteration={index} get_pixels")
                pixels = np.asarray(engine.get_pixels())
                if pixels.shape != (240, 320, 4):
                    raise RuntimeError(f"unexpected framebuffer shape {pixels.shape}")
                path = args.output_dir / f"screenshot_{index}.png"
                phase(f"iteration={index} save_screenshot")
                if not engine.save_screenshot(str(path)):
                    raise RuntimeError("save_screenshot failed")
                if not path.is_file() or path.stat().st_size == 0:
                    raise RuntimeError("screenshot missing or empty")
                with path.open("rb") as screenshot:
                    header = screenshot.read(24)
                if (header[:8] != b"\x89PNG\r\n\x1a\n"
                        or len(header) != 24
                        or struct.unpack(">II", header[16:24]) != (320, 240)):
                    raise RuntimeError("invalid PNG signature or screenshot dimensions")
            elif args.mode == "sensor":
                before = engine.frame_index, engine.scene_time
                phase(f"iteration={index} capture_sensor_outputs begin")
                outputs = engine.capture_sensor_outputs()
                if len(outputs) != args.cameras:
                    raise RuntimeError(f"expected {args.cameras} products, got {len(outputs)}")
                for output in outputs:
                    rgb = np.asarray(output.rgb)
                    depth = np.asarray(output.depth)
                    if rgb.shape != (240, 320, 4) or depth.shape[:2] != (240, 320):
                        raise RuntimeError(f"invalid output shapes: {rgb.shape}, {depth.shape}")
                    if not np.isfinite(depth).all():
                        raise RuntimeError("non-finite depth")
                if before != (engine.frame_index, engine.scene_time):
                    raise RuntimeError("sensor capture advanced frame identity or simulation time")
                retained.extend(outputs)
                phase(f"iteration={index} capture_sensor_outputs end")
        phase("shutdown begin (Python scene/entity/output handles retained)")
    phase("shutdown end")
    retained.clear()
    phase("retained handles released")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--scene", default="builtin:plane_cube")
    parser.add_argument("--mode", choices=("render", "readback", "sensor"), default="sensor")
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--captures", type=int, default=10)
    parser.add_argument("--cameras", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if min(args.runs, args.captures, args.cameras) < 1 or args.timeout <= 0:
        parser.error("runs, captures, cameras and timeout must be positive")
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    if args.worker:
        return worker(args)

    results = []
    for index in range(args.runs):
        run_dir = args.output_dir / f"run_{index:03d}"
        run_dir.mkdir(parents=True, exist_ok=True)
        command = [sys.executable, "-u", str(Path(__file__).resolve()),
                   "--worker", "--output-dir", str(run_dir), "--scene", args.scene,
                   "--mode", args.mode, "--captures", str(args.captures),
                   "--cameras", str(args.cameras)]
        if args.debug:
            command.append("--debug")
        start = time.monotonic()
        with (run_dir / "process.log").open("w", encoding="utf-8") as log:
            try:
                completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                           timeout=args.timeout, env=os.environ.copy())
                result = {"run": index, "returncode": completed.returncode,
                          "status": "passed" if completed.returncode == 0 else "failed"}
            except subprocess.TimeoutExpired:
                result = {"run": index, "returncode": None, "status": "timeout"}
        result["seconds"] = round(time.monotonic() - start, 3)
        results.append(result)
        phase(json.dumps(result))
        (args.output_dir / "results.json").write_text(
            json.dumps({"mode": args.mode, "scene": args.scene, "debug": args.debug,
                        "captures": args.captures, "cameras": args.cameras,
                        "results": results}, indent=2), encoding="utf-8")
    return int(any(result["status"] != "passed" for result in results))


if __name__ == "__main__":
    raise SystemExit(main())

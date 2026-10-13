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

    phase(f"native={caustica.__file__} backend={args.backend} mode={args.mode} scene={args.scene}")
    retained = []
    snapshots = []
    with caustica.EngineApp.create(
        width=args.width, height=args.height, headless=True, vulkan=args.backend == "vulkan",
        debug=args.debug, scene=args.scene, realtime=True,
    ) as engine:
        engine.settings.parallel_render_graph_recording = not args.serial_recording
        if args.fp32:
            engine.settings.use_fp16_types = False
        if args.no_denoiser:
            engine.settings.standalone_denoiser = False
            engine.settings.realtime_aa = 0
        if args.bounce_count is not None:
            engine.settings.bounce_count = args.bounce_count
        phase("created; waiting for scene")
        if not engine.wait_until_ready(timeout_seconds=120.0):
            raise RuntimeError("scene did not become ready")
        retained.append(engine.scene)
        if args.mode == "sensor":
            mask = int(caustica.Aov.rgb) | int(caustica.Aov.depth)
            for index in range(args.cameras):
                camera = engine.spawn_camera(
                    name=f"stress_{index}",
                    local_translation=(0.2 + index * 0.1, args.camera_height, 2.0),
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
                if pixels.shape != (args.height, args.width, 4):
                    raise RuntimeError(f"unexpected framebuffer shape {pixels.shape}")
                if index == 0:
                    snapshots.append((pixels, pixels.copy()))
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
                        or struct.unpack(">II", header[16:24]) != (args.width, args.height)):
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
                    if (rgb.shape != (args.height, args.width, 4)
                            or depth.shape[:2] != (args.height, args.width)):
                        raise RuntimeError(f"invalid output shapes: {rgb.shape}, {depth.shape}")
                    if not np.isfinite(depth).all():
                        raise RuntimeError("non-finite depth")
                    if index == 0:
                        snapshots.extend(((rgb, rgb.copy()), (depth, depth.copy())))
                if before != (engine.frame_index, engine.scene_time):
                    raise RuntimeError("sensor capture advanced frame identity or simulation time")
                retained.extend(outputs)
                phase(f"iteration={index} capture_sensor_outputs end")
        phase("shutdown begin (Python scene/entity/output handles retained)")
    phase("shutdown end")
    retained.clear()
    for actual, expected in snapshots:
        if not np.array_equal(actual, expected):
            raise RuntimeError("retained NumPy readback changed after later frames or engine shutdown")
    phase("retained handles released")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--scene", default="builtin:plane_cube")
    parser.add_argument("--backend", choices=("vulkan", "dx12"), default="vulkan")
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=240)
    parser.add_argument("--camera-height", type=float, default=1.0)
    parser.add_argument("--bounce-count", type=int)
    parser.add_argument("--mode", choices=("render", "readback", "sensor"), default="sensor")
    parser.add_argument("--runs", type=int, default=10)
    parser.add_argument("--captures", type=int, default=10)
    parser.add_argument("--cameras", type=int, default=2)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--serial-recording", action="store_true")
    parser.add_argument("--no-denoiser", action="store_true")
    parser.add_argument("--fp32", action="store_true")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if min(args.runs, args.captures, args.cameras, args.width, args.height) < 1 or args.timeout <= 0:
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
                   "--backend", args.backend,
                   "--worker", "--output-dir", str(run_dir), "--scene", args.scene,
                   "--mode", args.mode, "--captures", str(args.captures),
                   "--cameras", str(args.cameras), "--width", str(args.width),
                   "--height", str(args.height)]
        command.extend(["--camera-height", str(args.camera_height)])
        if args.bounce_count is not None:
            command.extend(["--bounce-count", str(args.bounce_count)])
        if args.debug:
            command.append("--debug")
        if args.serial_recording:
            command.append("--serial-recording")
        if args.no_denoiser:
            command.append("--no-denoiser")
        if args.fp32:
            command.append("--fp32")
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
            json.dumps({"backend": args.backend, "mode": args.mode, "scene": args.scene, "debug": args.debug,
                        "serial_recording": args.serial_recording, "no_denoiser": args.no_denoiser,
                        "width": args.width, "height": args.height,
                        "captures": args.captures, "cameras": args.cameras,
                        "results": results}, indent=2), encoding="utf-8")
    return int(any(result["status"] != "passed" for result in results))


if __name__ == "__main__":
    raise SystemExit(main())

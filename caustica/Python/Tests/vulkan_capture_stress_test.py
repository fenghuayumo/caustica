#!/usr/bin/env python3
"""Isolate repeated GPU runs; retain phase logs even on a crash or timeout."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import sys

from binding_regression_suite import run_case


def dense_scene(grid: int) -> str:
    from sensor_warmup_test import make_cube_scene

    entities = [{"id": "Ground", "name": "Ground",
                 "components": {"PrefabInstance": {"source": "builtin:plane"}}}]
    # Include the central cube from the original reported reproducer.
    for x, z in [(0.0, 0.0)] + [((i - grid / 2) * 1.2, (j - grid / 2) * 1.2)
                                for i in range(grid) for j in range(grid)]:
        name = f"DenseCube_{len(entities)}"
        entities.append({"id": name, "name": name, "components": {
            "PrefabInstance": {"source": "builtin:cube"},
            "Transform": {"translation": [x, 0.5, z]}}})
    entities.extend(entity for entity in json.loads(make_cube_scene())["entities"]
                    if entity["id"] == "Sun")
    return json.dumps({"settings": {"realtimeMode": True}, "entities": entities})


def phase(message: str) -> None:
    print(message, flush=True)


def worker(args) -> int:
    import caustica
    import numpy as np

    scene = args.scene
    if args.dense_grid:
        scene_path = args.output_dir / "dense.scene.json"
        scene_path.write_text(dense_scene(args.dense_grid), encoding="utf-8")
        scene = str(scene_path)
    phase(f"native={caustica.__file__} backend={args.backend} mode={args.mode} scene={scene}")
    retained = []
    snapshots = []
    with caustica.EngineApp.create(
        width=args.width, height=args.height, headless=True, vulkan=args.backend == "vulkan",
        debug=args.debug, scene=scene, realtime=not args.reference,
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
        churn_camera = None
        for index in range(args.captures):
            if args.churn_camera:
                if churn_camera is not None and not engine.despawn(churn_camera):
                    raise RuntimeError("despawn failed")
                churn_camera = engine.spawn_camera(name=f"churn_{index}",
                                                  local_translation=(0.2, 1.0, 2.0))
                if churn_camera is None:
                    raise RuntimeError("camera churn failed")
                retained.append(churn_camera)
            phase(f"iteration={index} step")
            if not engine.step_frame():
                raise RuntimeError("step_frame failed")
            if args.mode == "readback":
                phase(f"iteration={index} get_pixels")
                pixels = np.asarray(engine.get_pixels())
                if pixels.shape != (args.height, args.width, 4):
                    raise RuntimeError(f"unexpected framebuffer shape {pixels.shape}")
                if args.check_content and not np.any(pixels[..., :3]):
                    raise RuntimeError("empty framebuffer RGB output")
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
                from PIL import Image
                with Image.open(path) as screenshot:
                    saved = np.asarray(screenshot.convert("RGBA"))
                if not np.array_equal(saved, pixels):
                    raise RuntimeError("PNG pixels differ from framebuffer readback")
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
                    if args.check_content and (not np.any(rgb[..., :3]) or not np.any(depth > 0)):
                        raise RuntimeError(f"empty RGB/depth output: rgb_max={rgb[..., :3].max()}, "
                                           f"depth_max={depth.max()}")
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
    parser.add_argument("--reference", action="store_true")
    parser.add_argument("--dense-grid", type=int, default=0,
                        help="Generate a plane and N x N cubes; 16 reproduces the dense fault.")
    parser.add_argument("--churn-camera", action="store_true",
                        help="Spawn/despawn a camera each tick to exercise structure refreshes.")
    parser.add_argument("--check-content", action="store_true")
    parser.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if min(args.runs, args.captures, args.cameras, args.width, args.height) < 1 or args.timeout <= 0:
        parser.error("runs, captures, cameras and timeout must be positive")
    if args.dense_grid < 0:
        parser.error("dense-grid must be nonnegative")
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
        if args.reference:
            command.append("--reference")
        command.extend(["--dense-grid", str(args.dense_grid)])
        if args.churn_camera:
            command.append("--churn-camera")
        if args.check_content:
            command.append("--check-content")
        result = run_case(command, run_dir, args.timeout)
        result["run"] = index
        results.append(result)
        phase(json.dumps({key: result[key] for key in ("run", "returncode", "status", "seconds")}))
        (args.output_dir / "results.json").write_text(
            json.dumps({"backend": args.backend, "mode": args.mode, "scene": args.scene, "debug": args.debug,
                        "serial_recording": args.serial_recording, "no_denoiser": args.no_denoiser,
                        "reference": args.reference, "fp32": args.fp32,
                        "dense_grid": args.dense_grid, "churn_camera": args.churn_camera,
                        "check_content": args.check_content,
                        "width": args.width, "height": args.height,
                        "captures": args.captures, "cameras": args.cameras,
                        "results": results}, indent=2), encoding="utf-8")
    return int(any(result["status"] != "passed" for result in results))


if __name__ == "__main__":
    raise SystemExit(main())

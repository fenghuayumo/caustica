#!/usr/bin/env python3
"""P0-1: warmup / wait_until_ready bake, and teleport residual after reset.

1. realtime create → wait_until_ready() → get_pixels() is not a black frame.
2. yaw jump + reset_temporal_history + warmup(16) matches a clean pose render.
3. warmup does not advance scene_time (host physics snapshot stays put).
"""

from __future__ import annotations

import argparse
import json
import math
import os
import traceback
from pathlib import Path

from openpbr_white_furnace_render_test import write_rgba8_png


def log(message: str) -> None:
    os.write(1, (message + "\n").encode("utf-8", errors="replace"))


def yaw_xyzw(degrees: float) -> tuple[float, float, float, float]:
    half = math.radians(degrees) * 0.5
    return (0.0, math.sin(half), 0.0, math.cos(half))


def make_cube_scene() -> str:
    return json.dumps(
        {
            "settings": {"realtimeMode": True},
            "entities": [
                {
                    "id": "Ground",
                    "name": "Ground",
                    "components": {
                        "PrefabInstance": {"source": "builtin:plane"},
                    },
                },
                {
                    "id": "Cube",
                    "name": "Cube",
                    "components": {
                        "PrefabInstance": {"source": "builtin:cube"},
                        "Transform": {"translation": [0.0, 0.5, 0.0]},
                    },
                },
                {
                    "id": "Sun",
                    "name": "Sun",
                    "components": {
                        "Transform": {
                            "rotation": [-0.23053891, -0.15879166, -0.6890466, 0.6684697],
                        },
                        "DirectionalLight": {
                            "angularSize": 1.5,
                            "color": [1.0, 0.96, 0.9],
                            "irradiance": 4.0,
                        },
                    },
                },
                {
                    "id": "Cam",
                    "name": "Cam",
                    "components": {
                        "Transform": {"translation": [0.0, 1.2, 4.0]},
                        "PerspectiveCameraEx": {
                            "verticalFov": math.radians(35.0),
                            "zNear": 0.001,
                            "exposureCompensation": 0.0,
                            "enableAutoExposure": False,
                        },
                    },
                },
            ],
        }
    )


def as_uint8_rgb(pixels):
    import numpy as np

    return np.asarray(pixels, dtype=np.uint8)


def mean_rgb(pixels) -> float:
    return float(as_uint8_rgb(pixels)[..., :3].mean())


def mae_rgb(a, b) -> float:
    import numpy as np

    left = as_uint8_rgb(a)[..., :3].astype(np.float32)
    right = as_uint8_rgb(b)[..., :3].astype(np.float32)
    return float(np.abs(left - right).mean())


def set_cube_yaw(engine, degrees: float) -> None:
    cube = engine.find_entity("Cube")
    if cube is None:
        raise RuntimeError("Cube entity missing")
    translation, _rotation, scaling = cube.world_pose
    cube.set_world_pose(translation, yaw_xyzw(degrees), scaling)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--height", type=int, default=192)
    parser.add_argument("--min-mean-rgb", type=float, default=8.0)
    parser.add_argument("--max-teleport-mae", type=float, default=12.0)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    import caustica

    failures: list[str] = []

    with caustica.EngineApp.create(
        width=args.width,
        height=args.height,
        headless=True,
        scene="builtin:plane_cube",
        realtime=True,
    ) as engine:
        if not engine.is_scene_ready and not engine.wait_until_ready(timeout_seconds=120.0):
            raise RuntimeError("plane_cube scene did not become ready")

        pixels = as_uint8_rgb(engine.get_pixels())
        mean = mean_rgb(pixels)
        write_rgba8_png(
            args.output_dir / "realtime_wait_until_ready.png",
            int(pixels.shape[1]),
            int(pixels.shape[0]),
            pixels.tobytes(),
        )
        log(f"wait_until_ready mean RGB = {mean:.3f}")
        if mean < args.min_mean_rgb:
            failures.append(
                f"realtime wait_until_ready frame too dark: mean RGB {mean:.3f} < {args.min_mean_rgb}"
            )

        t0 = engine.scene_time
        engine.warmup(8)
        t1 = engine.scene_time
        if abs(t1 - t0) > 1e-9:
            failures.append(f"warmup advanced scene_time from {t0} to {t1}")

    with caustica.EngineApp.create(
        width=args.width,
        height=args.height,
        headless=True,
        scene=make_cube_scene(),
        realtime=True,
    ) as engine:
        if not engine.is_scene_ready and not engine.wait_until_ready(
            timeout_seconds=120.0, warmup_frames=16
        ):
            raise RuntimeError("cube scene did not become ready")

        cam = engine.find_entity("Cam")
        if cam is None:
            raise RuntimeError("Cam entity missing")
        cam.activate()
        engine.set_camera_pos_dir_up((0.0, 1.2, 4.0), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0))

        set_cube_yaw(engine, 25.0)
        engine.reset_temporal_history()
        engine.warmup(16)
        clean = as_uint8_rgb(engine.get_pixels()).copy()
        write_rgba8_png(
            args.output_dir / "pose_b_clean.png",
            int(clean.shape[1]),
            int(clean.shape[0]),
            clean.tobytes(),
        )
        clean_mean = mean_rgb(clean)
        log(f"pose_b_clean mean RGB = {clean_mean:.3f}")
        if clean_mean < args.min_mean_rgb:
            failures.append(
                f"teleport reference frame too dark: mean RGB {clean_mean:.3f} < {args.min_mean_rgb}"
            )

        set_cube_yaw(engine, 0.0)
        engine.reset_temporal_history()
        engine.warmup(16)
        set_cube_yaw(engine, 25.0)
        engine.reset_temporal_history()
        engine.warmup(16)
        after_teleport = as_uint8_rgb(engine.get_pixels()).copy()
        write_rgba8_png(
            args.output_dir / "pose_b_after_teleport.png",
            int(after_teleport.shape[1]),
            int(after_teleport.shape[0]),
            after_teleport.tobytes(),
        )

        mae = mae_rgb(clean, after_teleport)
        log(f"teleport+reset MAE = {mae:.3f}")
        if mae > args.max_teleport_mae:
            failures.append(
                f"yaw teleport left residual: MAE {mae:.3f} > {args.max_teleport_mae}"
            )

    if failures:
        for item in failures:
            log(f"FAIL: {item}")
        return 1
    log("sensor_warmup_test: ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        traceback.print_exc()
        raise SystemExit(1)

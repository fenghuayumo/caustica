#!/usr/bin/env python3
"""P0-5: batched set_world_poses matches per-entity writes; numpy vertices match lists."""

from __future__ import annotations

import argparse
import json
import math
import os
import traceback
from pathlib import Path

from openpbr_white_furnace_render_test import write_rgba8_png
from sensor_warmup_test import as_uint8_rgb, mean_rgb, yaw_xyzw


def log(message: str) -> None:
    os.write(1, (message + "\n").encode("utf-8", errors="replace"))


def mae_rgb(a, b) -> float:
    import numpy as np

    left = as_uint8_rgb(a)[..., :3].astype(np.float32)
    right = as_uint8_rgb(b)[..., :3].astype(np.float32)
    return float(np.abs(left - right).mean())


def make_cubes_scene(count: int) -> str:
    cols = 10
    entities = [
        {
            "id": "Ground",
            "name": "Ground",
            "components": {"PrefabInstance": {"source": "builtin:plane"}},
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
                "Transform": {"translation": [6.75, 10.0, 22.0]},
                "PerspectiveCameraEx": {
                    "verticalFov": math.radians(35.0),
                    "zNear": 0.001,
                    "exposureCompensation": 0.0,
                    "enableAutoExposure": False,
                },
            },
        },
    ]
    for i in range(count):
        x = float(i % cols) * 1.5
        z = float(i // cols) * 1.5
        entities.append(
            {
                "id": f"Cube_{i:03d}",
                "name": f"Cube_{i:03d}",
                "components": {
                    "PrefabInstance": {"source": "builtin:cube"},
                    "Transform": {"translation": [x, 0.5, z]},
                },
            }
        )
    return json.dumps({"settings": {"realtimeMode": True}, "entities": entities})


def capture(engine) -> object:
    engine.reset_temporal_history()
    engine.warmup(8)
    return as_uint8_rgb(engine.get_pixels()).copy()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--height", type=int, default=192)
    parser.add_argument("--max-pose-mae", type=float, default=40.0)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    import numpy as np
    import caustica

    failures: list[str] = []
    names = [f"Cube_{i:03d}" for i in range(args.count)]
    translations = np.zeros((args.count, 3), dtype=np.float32)
    rotations = np.zeros((args.count, 4), dtype=np.float32)
    rotations[:, 3] = 1.0
    cols = 10
    for i in range(args.count):
        translations[i, 0] = float(i % cols) * 1.5 + 0.4
        translations[i, 1] = 0.5
        translations[i, 2] = float(i // cols) * 1.5

    with caustica.EngineApp.create(
        width=args.width,
        height=args.height,
        headless=True,
        scene=make_cubes_scene(args.count),
        realtime=True,
    ) as engine:
        if not engine.is_scene_ready and not engine.wait_until_ready(timeout_seconds=180.0):
            raise RuntimeError("cube grid scene did not become ready")

        cam = engine.find_entity("Cam")
        if cam is None:
            raise RuntimeError("Cam missing")
        cam.activate()
        engine.set_camera_pos_dir_up((6.75, 10.0, 22.0), (0.0, -0.4, -1.0), (0.0, 1.0, 0.0))

        engine.set_world_poses(names, translations, rotations)
        batch_sample = engine.find_entity("Cube_000").world_pose
        batch_mid = engine.find_entity("Cube_050").world_pose
        batch_pixels = capture(engine)
        write_rgba8_png(
            args.output_dir / "batch_poses.png",
            int(batch_pixels.shape[1]),
            int(batch_pixels.shape[0]),
            batch_pixels.tobytes(),
        )
        log(f"batch mean RGB = {mean_rgb(batch_pixels):.3f}")
        if mean_rgb(batch_pixels) < 4.0:
            failures.append(f"batched pose frame too dark: mean {mean_rgb(batch_pixels):.3f}")

        identity = yaw_xyzw(0.0)
        for i, name in enumerate(names):
            entity = engine.find_entity(name)
            if entity is None:
                raise RuntimeError(f"missing {name}")
            entity.set_world_pose(
                (float(i % cols) * 1.5, 0.5, float(i // cols) * 1.5),
                identity,
            )

        for i, name in enumerate(names):
            entity = engine.find_entity(name)
            entity.set_world_pose(
                (float(translations[i, 0]), float(translations[i, 1]), float(translations[i, 2])),
                (
                    float(rotations[i, 0]),
                    float(rotations[i, 1]),
                    float(rotations[i, 2]),
                    float(rotations[i, 3]),
                ),
            )
        loop_pixels = capture(engine)
        write_rgba8_png(
            args.output_dir / "loop_poses.png",
            int(loop_pixels.shape[1]),
            int(loop_pixels.shape[0]),
            loop_pixels.tobytes(),
        )
        loop_sample = engine.find_entity("Cube_000").world_pose
        loop_mid = engine.find_entity("Cube_050").world_pose
        pose_mae = mae_rgb(batch_pixels, loop_pixels)
        log(f"batch vs loop MAE = {pose_mae:.3f}")
        log(f"Cube_000 batch={batch_sample[0]} loop={loop_sample[0]}")
        if not np.allclose(batch_sample[0], loop_sample[0], atol=1e-4) or not np.allclose(
            batch_mid[0], loop_mid[0], atol=1e-4
        ):
            failures.append("set_world_poses world translation diverged from set_world_pose loop")
        if pose_mae > args.max_pose_mae:
            failures.append(
                f"set_world_poses image diverged from set_world_pose loop: MAE {pose_mae:.3f} > {args.max_pose_mae}"
            )

        cube = engine.find_entity("Cube_000")
        if cube is None:
            raise RuntimeError("Cube_000 missing")
        original = engine.get_mesh_vertices(cube)
        array = np.asarray(original, dtype=np.float32)
        bumped = array.copy()
        bumped[:, 1] += 0.15
        engine.set_mesh_vertices(cube, bumped, space="object")
        numpy_roundtrip = np.asarray(engine.get_mesh_vertices(cube), dtype=np.float32)
        if numpy_roundtrip.shape != bumped.shape or not np.allclose(numpy_roundtrip, bumped, atol=1e-5):
            failures.append("numpy set_mesh_vertices did not round-trip vertex positions")

        engine.set_mesh_vertices(cube, original)
        engine.set_mesh_vertices(cube, bumped.tolist())
        list_roundtrip = np.asarray(engine.get_mesh_vertices(cube), dtype=np.float32)
        if not np.allclose(list_roundtrip, numpy_roundtrip, atol=1e-5):
            failures.append("list set_mesh_vertices diverged from numpy path")

        snapshot_t = translations.copy()
        snapshot_t[:, 0] += 0.2
        rigids = {
            name: (snapshot_t[i].tolist(), rotations[i].tolist())
            for i, name in enumerate(names)
        }
        engine.apply_visual_snapshot(rigids)
        pose = engine.find_entity("Cube_000").world_pose
        if abs(pose[0][0] - float(snapshot_t[0, 0])) > 1e-4:
            failures.append("apply_visual_snapshot did not write Cube_000 translation")

    if failures:
        for item in failures:
            log(f"FAIL: {item}")
        return 1
    log("sensor_world_poses_test: ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        traceback.print_exc()
        raise SystemExit(1)

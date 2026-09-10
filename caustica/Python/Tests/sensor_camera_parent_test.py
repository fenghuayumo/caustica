#!/usr/bin/env python3
"""P0-4: parented wrist camera follows the parent without look_to."""

from __future__ import annotations

import argparse
import math
import os
import traceback
from pathlib import Path

from openpbr_white_furnace_render_test import write_rgba8_png
from sensor_warmup_test import as_uint8_rgb, make_cube_scene, mean_rgb, yaw_xyzw


def log(message: str) -> None:
    os.write(1, (message + "\n").encode("utf-8", errors="replace"))


def mae_rgb(a, b) -> float:
    import numpy as np

    left = as_uint8_rgb(a)[..., :3].astype(np.float32)
    right = as_uint8_rgb(b)[..., :3].astype(np.float32)
    return float(np.abs(left - right).mean())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--height", type=int, default=192)
    parser.add_argument("--min-view-mae", type=float, default=12.0)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    import numpy as np
    import caustica

    failures: list[str] = []
    with caustica.EngineApp.create(
        width=args.width,
        height=args.height,
        headless=True,
        scene=make_cube_scene(),
        realtime=True,
    ) as engine:
        if not engine.is_scene_ready and not engine.wait_until_ready(timeout_seconds=120.0):
            raise RuntimeError("cube scene did not become ready")

        cube = engine.find_entity("Cube")
        if cube is None:
            raise RuntimeError("Cube entity missing")

        cam = engine.spawn_camera(
            name="wrist",
            parent=cube,
            local_translation=(0.0, 0.8, 3.0),
            local_rotation=(0.0, 0.0, 0.0, 1.0),
            vertical_fov=math.radians(35.0),
        )
        if cam.parent is None or cam.parent.name != "Cube":
            failures.append(f"spawned camera parent is {cam.parent}, expected Cube")

        local_before = cam.local_pose
        aovs = int(caustica.Aov.rgb) | int(caustica.Aov.instance_id)
        engine.add_render_product("wrist", cam, aovs)
        engine.reset_temporal_history()
        engine.warmup(12)
        products = {item.name: item for item in engine.capture_sensor_outputs()}
        before = products["wrist"]
        before_rgb = as_uint8_rgb(before.rgb).copy()
        write_rgba8_png(
            args.output_dir / "parent_yaw0.png",
            int(before_rgb.shape[1]),
            int(before_rgb.shape[0]),
            before_rgb.tobytes(),
        )
        log(f"yaw0 mean RGB = {mean_rgb(before_rgb):.3f}")
        if mean_rgb(before_rgb) < 4.0:
            failures.append(f"parented camera frame too dark: mean {mean_rgb(before_rgb):.3f}")

        translation, _rotation, scaling = cube.world_pose
        cube.set_world_pose(translation, yaw_xyzw(90.0), scaling)
        local_after_parent_move = cam.local_pose
        if local_after_parent_move[0] != local_before[0]:
            failures.append("camera local translation changed when the parent rotated")

        engine.reset_temporal_history()
        engine.warmup(12)
        products = {item.name: item for item in engine.capture_sensor_outputs()}
        after = products["wrist"]
        after_rgb = as_uint8_rgb(after.rgb).copy()
        write_rgba8_png(
            args.output_dir / "parent_yaw90.png",
            int(after_rgb.shape[1]),
            int(after_rgb.shape[0]),
            after_rgb.tobytes(),
        )
        view_mae = mae_rgb(before_rgb, after_rgb)
        log(f"parent yaw 90 MAE = {view_mae:.3f}")
        if view_mae < args.min_view_mae:
            failures.append(
                f"parent yaw 90 did not change the wrist view: MAE {view_mae:.3f} < {args.min_view_mae}"
            )

        if before.instance_id is not None and after.instance_id is not None:
            before_ids = set(np.unique(np.asarray(before.instance_id)).tolist()) - {0}
            after_ids = set(np.unique(np.asarray(after.instance_id)).tolist()) - {0}
            log(f"instance ids before={sorted(before_ids)} after={sorted(after_ids)}")
            if before_ids and after_ids and before_ids.isdisjoint(after_ids):
                failures.append("instance ids diverged after parent yaw; gripper correspondence lost")

        world_cam = engine.spawn_camera(name="world_cam", parent=None)
        world_cam.set_parent(cube)
        if world_cam.parent is None or world_cam.parent.name != "Cube":
            failures.append("set_parent(cube) did not attach world_cam under Cube")
        world_cam.set_parent(None)
        if world_cam.parent is not None:
            failures.append("set_parent(None) did not return world_cam to the scene root")

    if failures:
        for item in failures:
            log(f"FAIL: {item}")
        return 1
    log("sensor_camera_parent_test: ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        traceback.print_exc()
        raise SystemExit(1)

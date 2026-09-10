#!/usr/bin/env python3
"""P1: URDF visual FK (scheme A) and set_mesh_triangles topology rebuild."""

from __future__ import annotations

import argparse
import math
import os
import traceback
from pathlib import Path

from openpbr_white_furnace_render_test import write_rgba8_png
from sensor_warmup_test import as_uint8_rgb, make_cube_scene, mean_rgb


def log(message: str) -> None:
    os.write(1, (message + "\n").encode("utf-8", errors="replace"))


def mae_rgb(a, b) -> float:
    import numpy as np

    left = as_uint8_rgb(a)[..., :3].astype(np.float32)
    right = as_uint8_rgb(b)[..., :3].astype(np.float32)
    return float(np.abs(left - right).mean())


TWO_LINK_URDF = """<?xml version="1.0"?>
<robot name="p1_arm">
  <link name="base_link">
    <visual>
      <geometry><box size="0.16 0.16 0.16"/></geometry>
      <material name="base"><color rgba="0.75 0.75 0.8 1"/></material>
    </visual>
  </link>
  <link name="link1">
    <visual>
      <origin xyz="0.14 0 0"/>
      <geometry><box size="0.28 0.08 0.08"/></geometry>
      <material name="l1"><color rgba="0.85 0.25 0.2 1"/></material>
    </visual>
  </link>
  <link name="link2">
    <visual>
      <origin xyz="0.14 0 0"/>
      <geometry><box size="0.28 0.07 0.07"/></geometry>
      <material name="l2"><color rgba="0.2 0.55 0.85 1"/></material>
    </visual>
  </link>
  <joint name="joint1" type="revolute">
    <parent link="base_link"/>
    <child link="link1"/>
    <origin xyz="0 0.12 0" rpy="0 0 0"/>
    <axis xyz="0 1 0"/>
  </joint>
  <joint name="joint2" type="revolute">
    <parent link="link1"/>
    <child link="link2"/>
    <origin xyz="0.28 0 0" rpy="0 0 0"/>
    <axis xyz="0 1 0"/>
  </joint>
</robot>
"""


def pose_close(actual, expected, atol: float) -> bool:
    import numpy as np

    return np.allclose(actual, expected, atol=atol)


def capture_product(engine, name: str):
    engine.reset_temporal_history()
    engine.warmup(10)
    products = {item.name: item for item in engine.capture_sensor_outputs()}
    return products[name]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--height", type=int, default=192)
    parser.add_argument("--min-fk-mae", type=float, default=8.0)
    parser.add_argument("--min-tri-mae", type=float, default=8.0)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    import numpy as np
    import caustica

    urdf_path = args.output_dir / "p1_arm.urdf"
    urdf_path.write_text(TWO_LINK_URDF, encoding="utf-8")

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

        original_verts = np.asarray(engine.get_mesh_vertices(cube), dtype=np.float32)
        if original_verts.shape[0] < 3:
            raise RuntimeError(f"Cube unique vertex count too small: {original_verts.shape}")

        aovs = int(caustica.Aov.rgb)
        engine.add_render_product("main", engine.find_entity("Cam"), aovs)
        before_tris = as_uint8_rgb(capture_product(engine, "main").rgb).copy()
        write_rgba8_png(
            args.output_dir / "tris_before.png",
            int(before_tris.shape[1]),
            int(before_tris.shape[0]),
            before_tris.tobytes(),
        )

        faces = np.array([[0, 1, 2]], dtype=np.uint32)
        try:
            engine.set_mesh_triangles(cube, faces)
        except Exception as exc:
            failures.append(f"set_mesh_triangles raised: {exc}")
        else:
            roundtrip = np.asarray(engine.get_mesh_vertices(cube), dtype=np.float32)
            if roundtrip.shape[0] < 3:
                failures.append("set_mesh_triangles collapsed the cube below 3 vertices")
            after_tris = as_uint8_rgb(capture_product(engine, "main").rgb).copy()
            write_rgba8_png(
                args.output_dir / "tris_after.png",
                int(after_tris.shape[1]),
                int(after_tris.shape[0]),
                after_tris.tobytes(),
            )
            tri_mae = mae_rgb(before_tris, after_tris)
            log(f"triangle topology MAE = {tri_mae:.3f}")
            if tri_mae < args.min_tri_mae:
                failures.append(
                    f"set_mesh_triangles did not change the image: MAE {tri_mae:.3f} < {args.min_tri_mae}"
                )
            try:
                engine.set_mesh_triangles(cube, np.array([[0, 1, original_verts.shape[0] + 5]], dtype=np.uint32))
                failures.append("out-of-range triangle index did not raise")
            except Exception:
                pass
            cube.set_world_pose((20.0, 0.5, 0.0), (0.0, 0.0, 0.0, 1.0))

        robot = engine.spawn_from_file(str(urdf_path))
        if robot is None:
            raise RuntimeError("spawn_from_file(urdf) failed")
        if robot.joint_names != ["joint1", "joint2"]:
            failures.append(f"joint_names={robot.joint_names}, expected ['joint1', 'joint2']")

        t1, q1 = robot.get_link_pose("link1")
        t2, q2 = robot.get_link_pose("link2")
        log(f"rest link1 t={t1} q={q1}")
        log(f"rest link2 t={t2} q={q2}")
        if not pose_close(t1, (0.0, 0.12, 0.0), 1e-4):
            failures.append(f"rest link1 translation {t1} != (0, 0.12, 0)")
        if not pose_close(t2, (0.28, 0.12, 0.0), 1e-4):
            failures.append(f"rest link2 translation {t2} != (0.28, 0.12, 0)")
        if not pose_close(q1, (0.0, 0.0, 0.0, 1.0), 1e-4) or not pose_close(q2, (0.0, 0.0, 0.0, 1.0), 1e-4):
            failures.append("rest link rotations were not identity")

        half = math.pi * 0.25
        robot.set_joint_positions(np.array([math.pi * 0.5, 0.0], dtype=np.float32))
        t1b, q1b = robot.get_link_pose("link1")
        t2b, q2b = robot.get_link_pose("link2")
        log(f"q=90 link1 t={t1b} q={q1b}")
        log(f"q=90 link2 t={t2b} q={q2b}")
        if not pose_close(t1b, (0.0, 0.12, 0.0), 1e-4):
            failures.append(f"revolute joint1 moved link1 translation to {t1b}")
        if not pose_close(t2b, (0.0, 0.12, -0.28), 1e-4):
            failures.append(f"FK link2 translation {t2b} != (0, 0.12, -0.28)")
        if not pose_close(q1b, (0.0, math.sin(half), 0.0, math.cos(half)), 1e-4):
            failures.append(f"FK link1 rotation {q1b} != Ry(90)")

        robot.set_joint_positions([0.0, 0.0])
        wrist = robot.attach_camera(
            "wrist",
            link="link2",
            local_t=(0.0, 0.12, 0.55),
            local_q=(0.0, 0.0, 0.0, 1.0),
        )
        if wrist.parent is None or wrist.parent.name != "link2":
            failures.append(f"attach_camera parent is {wrist.parent}, expected link2")

        engine.add_render_product("wrist", wrist, aovs)
        rest_view = as_uint8_rgb(capture_product(engine, "wrist").rgb).copy()
        write_rgba8_png(
            args.output_dir / "fk_q0.png",
            int(rest_view.shape[1]),
            int(rest_view.shape[0]),
            rest_view.tobytes(),
        )
        log(f"wrist q0 mean RGB = {mean_rgb(rest_view):.3f}")
        if mean_rgb(rest_view) < 2.0:
            failures.append(f"wrist rest frame too dark: mean {mean_rgb(rest_view):.3f}")

        robot.set_joint_positions(np.array([math.pi * 0.5, 0.0], dtype=np.float32))
        posed_view = as_uint8_rgb(capture_product(engine, "wrist").rgb).copy()
        write_rgba8_png(
            args.output_dir / "fk_q90.png",
            int(posed_view.shape[1]),
            int(posed_view.shape[0]),
            posed_view.tobytes(),
        )
        fk_mae = mae_rgb(rest_view, posed_view)
        log(f"wrist FK MAE = {fk_mae:.3f}")
        if fk_mae < args.min_fk_mae:
            failures.append(
                f"set_joint_positions did not change the wrist view: MAE {fk_mae:.3f} < {args.min_fk_mae}"
            )

        local_before = wrist.local_pose
        robot.set_joint_positions(np.array([0.0, math.pi * 0.25], dtype=np.float32))
        if local_before[0] != wrist.local_pose[0]:
            failures.append("wrist local translation changed when the parent joint moved")

    if failures:
        for item in failures:
            log(f"FAIL: {item}")
        return 1
    log("sensor_robot_fk_test: ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        traceback.print_exc()
        raise SystemExit(1)

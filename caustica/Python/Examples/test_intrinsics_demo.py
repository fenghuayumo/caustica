#!/usr/bin/env python
"""Demonstrate set_camera_intrinsics with centered vs off-center principal points.

Usage:
    python caustica/Python/Examples/test_intrinsics_demo.py --out-dir ./intrinsics_test
"""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np

from _common import resolve_output_path


def build_test_scene(caustica) -> str:
    if hasattr(caustica, "builtin_scene_json"):
        # Prefer builtin helper, then strip the camera so we set pose/intrinsics in code.
        # Fall back to a minimal inline scene without a camera node.
        pass
    scene = {
        "models": ["builtin:plane_cube"],
        "graph": [
            {
                "name": "Ground",
                "model": 0,
                "translation": [0.0, 0.0, 0.0],
                "scaling": [2.0, 1.0, 2.0],
            },
            {
                "name": "Lights",
                "children": [
                    {
                        "name": "Sun",
                        "type": "DirectionalLight",
                        "rotation": [-0.23053891, -0.15879166, -0.68904659, 0.66846975],
                        "angularSize": 1.5,
                        "color": [1.0, 0.96, 0.9],
                        "irradiance": 3.0,
                    }
                ],
            },
            {
                "name": "SampleSettings",
                "type": "SampleSettings",
                "realtimeMode": False,
                "startingCamera": -1,
            },
        ],
    }
    import json

    return json.dumps(scene, indent=2)


def build_camera_pose(position, look_at, up) -> np.ndarray:
    pos = np.asarray(position, dtype=np.float64)
    target = np.asarray(look_at, dtype=np.float64)
    up_vec = np.asarray(up, dtype=np.float64)
    forward = target - pos
    forward = forward / np.linalg.norm(forward)
    right = np.cross(forward, up_vec)
    right = right / np.linalg.norm(right)
    up_new = np.cross(right, forward)
    pose = np.eye(4, dtype=np.float64)
    pose[:3, 0] = right
    pose[:3, 1] = up_new
    pose[:3, 2] = forward
    pose[:3, 3] = pos
    return pose


def apply_intrinsics(renderer, width: int, height: int, fx: float, fy: float, cx: float, cy: float) -> None:
    pose = build_camera_pose([0.0, 1.6, 5.0], [0.0, 0.0, 0.0], [0.0, 1.0, 0.0])
    cam_pos = pose[:3, 3]
    cam_dir = pose[:3, 2]
    cam_up = pose[:3, 1]
    renderer.set_camera(cam_pos.tolist(), cam_dir.tolist(), cam_up.tolist())
    renderer.set_camera_intrinsics(fx, fy, cx, cy, float(width), float(height))
    print(f"[caustica] intrinsics fx={fx:.1f} fy={fy:.1f} cx={cx:.1f} cy={cy:.1f}")


def render_case(renderer, width, height, fx, fy, cx, cy, out_path: Path) -> bool:
    apply_intrinsics(renderer, width, height, fx, fy, cx, cy)
    renderer.settings.reset_accumulation = True
    frames = renderer.step_until_accumulated()
    ok = renderer.save_screenshot(str(out_path))
    print(f"[caustica] {'Saved' if ok else 'FAILED'}: {out_path} ({frames} frames)")
    return ok


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Test set_camera_intrinsics.")
    parser.add_argument("--out-dir", type=str, default="./intrinsics_test")
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--spp", type=int, default=16)
    parser.add_argument("--fx", type=float, default=346.0)
    parser.add_argument("--fy", type=float, default=346.0)
    parser.add_argument("--cx", type=float, default=320.0)
    parser.add_argument("--cy", type=float, default=240.0)
    parser.add_argument("--cx-offset", type=float, default=160.0)
    parser.add_argument("--cy-offset", type=float, default=120.0)
    parser.add_argument("--vulkan", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    launch_cwd = Path.cwd()
    import caustica

    out_dir = resolve_output_path(args.out_dir, launch_cwd)
    out_dir.mkdir(parents=True, exist_ok=True)

    with caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=True,
        vulkan=args.vulkan,
        scene=build_test_scene(caustica),
        realtime=False,
        accumulation_target=args.spp,
    ) as renderer:
        renderer.app.set_reference_mode(spp=args.spp, oidn=False)
        s = renderer.settings
        s.bounce_count = 4
        s.enable_tone_mapping = True

        ok1 = render_case(
            renderer, args.width, args.height, args.fx, args.fy, args.cx, args.cy,
            out_dir / "intrinsics_baseline.png",
        )
        ok2 = render_case(
            renderer, args.width, args.height, args.fx, args.fy, args.cx_offset, args.cy_offset,
            out_dir / "intrinsics_offset.png",
        )

    print("\nCompare baseline vs offset PNGs; offset cx/cy should shift the projection.")
    return 0 if (ok1 and ok2) else 1


if __name__ == "__main__":
    raise SystemExit(main())

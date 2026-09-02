#!/usr/bin/env python
"""Drive the camera from a pinhole intrinsic matrix instead of a vertical FOV.

``set_camera_intrinsics(fx, fy, cx, cy, width, height)`` builds an off-center
projection, which is what you need to reproduce a real or calibrated camera
whose principal point is not at the image center. This renders the same scene
twice -- once with a centered principal point and once shifted -- so the
resulting pair shows the projection shift directly.

Example:
    python examples/python/camera_intrinsics.py --out-dir intrinsics_out
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from _common import (
    add_device_args,
    import_caustica,
    make_engine,
    normalize,
    resolve_output_path,
    save_screenshot,
)

CAMERA_POSITION = (0.0, 1.6, 5.0)
LOOK_AT = (0.0, 0.0, 0.0)


def build_scene_json() -> str:
    """A lit ground plane and cube with no camera node, so the script owns the camera."""
    scene = {
        "settings": {"realtimeMode": False},
        "entities": [
            {
                "id": "Ground",
                "name": "Ground",
                "components": {
                    "Transform": {"scale": [2.0, 1.0, 2.0]},
                    "PrefabInstance": {"source": "builtin:plane_cube"},
                },
            },
            {"id": "Lights", "name": "Lights"},
            {
                "id": "Sun",
                "name": "Sun",
                "parent": "Lights",
                "components": {
                    "Transform": {
                        "rotation": [-0.23053891, -0.15879166, -0.68904659, 0.66846975]
                    },
                    "DirectionalLight": {
                        "angularSize": 1.5,
                        "color": [1.0, 0.96, 0.9],
                        "irradiance": 3.0,
                    },
                },
            },
        ],
    }
    return json.dumps(scene, indent=2)


def look_at(position, target, up=(0.0, 1.0, 0.0)):
    """Return the (direction, up) pair pointing from ``position`` to ``target``."""
    forward = normalize([target[i] - position[i] for i in range(3)])
    right = normalize(
        [
            forward[1] * up[2] - forward[2] * up[1],
            forward[2] * up[0] - forward[0] * up[2],
            forward[0] * up[1] - forward[1] * up[0],
        ]
    )
    true_up = (
        right[1] * forward[2] - right[2] * forward[1],
        right[2] * forward[0] - right[0] * forward[2],
        right[0] * forward[1] - right[1] * forward[0],
    )
    return forward, true_up


def render_case(engine, args, cx: float, cy: float, out_path: Path) -> Path:
    direction, up = look_at(CAMERA_POSITION, LOOK_AT)
    engine.set_camera_pos_dir_up(CAMERA_POSITION, direction, up)
    engine.set_camera_intrinsics(
        args.fx, args.fy, cx, cy, float(args.width), float(args.height)
    )
    print(f"[caustica] intrinsics fx={args.fx:.1f} fy={args.fy:.1f} cx={cx:.1f} cy={cy:.1f}")

    engine.settings.reset_accumulation = True
    frames = engine.step_until_accumulated()
    saved = save_screenshot(engine, out_path)
    print(f"[caustica] Saved {saved} ({frames} frames)")
    return saved


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--out-dir", default="intrinsics_out")
    parser.add_argument("--spp", type=int, default=16)
    parser.add_argument("--fx", type=float, default=346.0)
    parser.add_argument("--fy", type=float, default=346.0)
    parser.add_argument("--cx", type=float, default=320.0, help="Centered principal point x.")
    parser.add_argument("--cy", type=float, default=240.0, help="Centered principal point y.")
    parser.add_argument("--cx-offset", type=float, default=160.0, help="Shifted principal point x.")
    parser.add_argument("--cy-offset", type=float, default=120.0, help="Shifted principal point y.")
    add_device_args(parser, width=640, height=480)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    caustica = import_caustica()
    out_dir = resolve_output_path(args.out_dir)

    with make_engine(
        caustica,
        args,
        scene=build_scene_json(),
        realtime=False,
        headless=True,
        accumulation_target=args.spp,
    ) as engine:
        engine.set_reference_mode(spp=args.spp, oidn=False)
        engine.settings.bounce_count = 4
        engine.settings.enable_tone_mapping = True

        render_case(engine, args, args.cx, args.cy, out_dir / "intrinsics_centered.png")
        render_case(
            engine, args, args.cx_offset, args.cy_offset, out_dir / "intrinsics_offset.png"
        )

    print("\nCompare the two images: the offset principal point shifts the projection.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

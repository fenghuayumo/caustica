#!/usr/bin/env python
"""Headless reference rendering with the caustica Python extension.

Usage:
    python caustica/Python/Examples/offline_render.py \\
        --scene bistro-programmer-art.scene.json \\
        --width 1280 --height 720 --spp 256 --out frame.png
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from _common import resolve_output_path, resolve_scene_arg


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="caustica offline / headless reference render.")
    parser.add_argument("--scene", default="bistro-programmer-art.scene.json")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--spp", type=int, default=256)
    parser.add_argument("--bounces", type=int, default=8)
    parser.add_argument("--out", default="frame.png")
    parser.add_argument("--vulkan", action="store_true")
    parser.add_argument("--adapter", default="auto")
    parser.add_argument("--no-headless", dest="headless", action="store_false", default=True)
    parser.add_argument("--oidn", action="store_true")
    parser.add_argument("--oidn-gpu", dest="oidn_gpu", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--oidn-quality", type=int, default=2)
    parser.add_argument("--oidn-passes", type=int, default=2)
    parser.add_argument("--oidn-prefilter", type=int, default=2)
    parser.add_argument("--camera-pos", type=float, nargs=3, metavar=("X", "Y", "Z"))
    parser.add_argument("--camera-dir", type=float, nargs=3, metavar=("X", "Y", "Z"))
    parser.add_argument("--camera-up", type=float, nargs=3, metavar=("X", "Y", "Z"), default=(0.0, 1.0, 0.0))
    parser.add_argument("--fov", type=float, default=None)
    parser.add_argument(
        "--material-override",
        dest="material_overrides",
        action="append",
        nargs=4,
        metavar=("NAME", "R", "G", "B"),
    )
    parser.add_argument("--gaussian-splat", default=None)
    parser.add_argument("--gaussian-splat-convert-rdf-to-rub", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--gaussian-splat-depth-test", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--gaussian-splat-scale", type=float, default=1.0)
    parser.add_argument("--gaussian-splat-alpha-scale", type=float, default=1.0)
    parser.add_argument("--gaussian-splat-brightness", type=float, default=1.0)
    parser.add_argument("--gaussian-splat-alpha-cull", type=float, default=1.0 / 255.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    launch_cwd = Path.cwd()
    try:
        import caustica
    except ImportError as exc:
        sys.stderr.write(
            "Failed to import caustica. Install with: python -m pip install .\n"
            f"Original error: {exc}\n"
        )
        raise

    scene = resolve_scene_arg(args.scene)
    print(f"[caustica] Mode: {caustica.MODE}")
    print(f"[caustica] Creating Renderer ({args.width}x{args.height}, headless={args.headless})")

    with caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=args.headless,
        vulkan=args.vulkan,
        adapter=args.adapter,
        scene=scene,
        realtime=False,
        accumulation_target=args.spp,
    ) as renderer:
        app = renderer.app
        print(f"[caustica] Loaded scene: {app.scene_name}")
        print(f"[caustica] Materials: {app.scene.material_count}  Lights: {app.scene.light_count}")

        app.set_reference_mode(
            spp=args.spp,
            oidn=args.oidn,
            oidn_quality=args.oidn_quality,
            oidn_passes=args.oidn_passes,
            oidn_prefilter=args.oidn_prefilter,
        )
        s = renderer.settings
        s.bounce_count = args.bounces
        s.use_nee = True
        s.enable_tone_mapping = True
        s.use_restir_di = False
        s.use_restir_gi = False
        s.oidn_use_gpu = args.oidn_gpu
        if args.oidn:
            s.oidn_apply()

        if args.gaussian_splat:
            s.enable_gaussian_splats = True
            s.gaussian_splat_depth_test = args.gaussian_splat_depth_test
            s.gaussian_splat_scale = args.gaussian_splat_scale
            s.gaussian_splat_alpha_scale = args.gaussian_splat_alpha_scale
            s.gaussian_splat_brightness = args.gaussian_splat_brightness
            s.gaussian_splat_alpha_cull_threshold = args.gaussian_splat_alpha_cull
            if not renderer.load_gaussian_splats(
                args.gaussian_splat, args.gaussian_splat_convert_rdf_to_rub
            ):
                raise RuntimeError(f"Failed to load Gaussian splat: {args.gaussian_splat}")

        if args.camera_pos and args.camera_dir:
            renderer.set_camera(tuple(args.camera_pos), tuple(args.camera_dir), tuple(args.camera_up))
        if args.fov is not None:
            renderer.set_camera_fov(args.fov)

        if args.material_overrides:
            for name, r, g, b in args.material_overrides:
                mat = app.scene.find_material(name)
                if mat is None:
                    print(f"[caustica] WARNING: material '{name}' not found")
                    continue
                mat.base_color = (float(r), float(g), float(b))
                print(f"[caustica] Overrode '{name}' base_color")

        print(f"[caustica] Rendering {args.spp} spp ...")
        t0 = time.time()
        frames = renderer.step_until_accumulated()
        print(f"[caustica] Done in {time.time() - t0:.2f}s ({frames} frames)")

        out_path = resolve_output_path(args.out, launch_cwd)
        if not renderer.save_screenshot(str(out_path)):
            raise RuntimeError(f"Failed to save screenshot: {out_path}")
        print(f"[caustica] Saved: {out_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

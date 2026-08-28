#!/usr/bin/env python
"""Render a scene through the installed caustica Python extension.

This is the general-purpose starting point. It covers the three ways the
renderer can be driven -- reference accumulation, fixed-count realtime frames,
and an interactive window -- plus runtime scene edits and framebuffer readback.

Examples:
    python examples/python/render.py --scene builtin:plane_cube --out frame.png
    python examples/python/render.py --mode realtime --denoiser nrd --frames 32
    python examples/python/render.py --mode window --scene Assets/scenes/default/default.scene.json
    python examples/python/render.py --spawn Assets/models/GlassSphere/GlassSphere.gltf
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

from _common import (
    REALTIME_DENOISERS,
    add_device_args,
    add_quality_args,
    apply_common_settings,
    apply_realtime_mode,
    apply_reference_mode,
    frame_bounds,
    import_caustica,
    make_renderer,
    render_reference_to,
    require_input_file,
    resolve_scene_arg,
    run_window_loop,
    save_screenshot,
    scene_bounds_center_radius,
    validate_positive,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--scene",
        default="builtin:plane_cube",
        help="Scene JSON path, Assets-relative name, or builtin: reference.",
    )
    parser.add_argument(
        "--mode",
        choices=["reference", "realtime", "window"],
        default="reference",
        help="Reference accumulation, fixed-count realtime frames, or an interactive window.",
    )
    parser.add_argument("--out", default="frame.png")
    parser.add_argument(
        "--denoiser",
        choices=REALTIME_DENOISERS,
        default="auto",
        help="Realtime denoiser / AA path; auto selects NRD + TAA. Unsupported "
        "paths fall back automatically.",
    )
    parser.add_argument(
        "--oidn",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Denoise the reference render with OIDN.",
    )
    parser.add_argument("--frames", type=int, default=32, help="Realtime frames to render.")

    scene_edits = parser.add_argument_group("scene edits")
    scene_edits.add_argument("--spawn", help="Mesh or prefab to spawn after loading the scene.")
    scene_edits.add_argument(
        "--spawn-position", type=float, nargs=3, default=(0.0, 0.0, 0.0), metavar=("X", "Y", "Z")
    )
    scene_edits.add_argument(
        "--material",
        action="append",
        nargs=4,
        metavar=("NAME", "R", "G", "B"),
        help="Override a material base color; may be repeated.",
    )
    scene_edits.add_argument("--camera-pos", type=float, nargs=3, metavar=("X", "Y", "Z"))
    scene_edits.add_argument("--camera-dir", type=float, nargs=3, metavar=("X", "Y", "Z"))
    scene_edits.add_argument(
        "--camera-up", type=float, nargs=3, default=(0.0, 1.0, 0.0), metavar=("X", "Y", "Z")
    )
    scene_edits.add_argument("--fov", type=float)

    parser.add_argument(
        "--inspect-framebuffer",
        action="store_true",
        help="Read back and print RGBA8 framebuffer metadata.",
    )
    add_device_args(parser)
    add_quality_args(parser)
    return parser.parse_args()


def apply_scene_edits(renderer, args: argparse.Namespace) -> None:
    if args.spawn:
        spawn_path = require_input_file(args.spawn, "Spawn asset")
        entity = renderer.app.spawn_from_file(str(spawn_path))
        if entity is None:
            raise SystemExit(f"spawn_from_file failed: {spawn_path}")
        entity.translation = tuple(args.spawn_position)
        renderer.step_n(1)
        print(f"[caustica] Spawned {entity.name}: {spawn_path}")
        if args.camera_pos is None:
            framing = scene_bounds_center_radius(renderer)
            if framing is not None:
                frame_bounds(renderer, *framing)

    for name, red, green, blue in args.material or []:
        material = renderer.app.scene.find_material(name)
        if material is None:
            print(f"[caustica] WARNING: material not found: {name}")
            continue
        material.base_color = (float(red), float(green), float(blue))
        print(f"[caustica] Updated material: {name}")

    if bool(args.camera_pos) != bool(args.camera_dir):
        raise SystemExit("--camera-pos and --camera-dir must be provided together")
    if args.camera_pos and args.camera_dir:
        renderer.set_camera(
            tuple(args.camera_pos), tuple(args.camera_dir), tuple(args.camera_up)
        )
    if args.fov is not None:
        renderer.set_camera_fov(args.fov)


def inspect_framebuffer(renderer) -> None:
    framebuffer = renderer.get_framebuffer()
    pixels = framebuffer.pixels
    print(
        "[caustica] Framebuffer "
        f"shape={framebuffer.shape} format={framebuffer.format} "
        f"dtype={framebuffer.dtype} bytes={len(pixels)} "
        f"range=[{min(pixels)}, {max(pixels)}]"
    )


def main() -> int:
    args = parse_args()
    validate_positive(args, "width", "height", "spp", "frames")

    caustica = import_caustica()
    launch_cwd = Path.cwd()
    is_reference = args.mode == "reference"
    is_windowed = args.mode == "window"

    with make_renderer(
        caustica,
        args,
        scene=resolve_scene_arg(args.scene),
        realtime=not is_reference,
        headless=not is_windowed,
        accumulation_target=args.spp if is_reference else 1,
    ) as renderer:
        apply_common_settings(renderer, caustica, bounces=args.bounces)
        mode_label = (
            apply_reference_mode(renderer, caustica, spp=args.spp, oidn=args.oidn)
            if is_reference
            else apply_realtime_mode(renderer.app, caustica, args.denoiser)
        )
        apply_scene_edits(renderer, args)

        print(f"[caustica] Scene: {renderer.app.scene_name}")
        print(f"[caustica] Mode : {mode_label}")

        if is_windowed:
            run_window_loop(renderer)
            return 0

        if is_reference:
            if args.inspect_framebuffer:
                renderer.step_until_accumulated()
                inspect_framebuffer(renderer)
                save_screenshot(renderer, args.out, launch_cwd=launch_cwd)
            else:
                render_reference_to(renderer, args.out, launch_cwd=launch_cwd, label=mode_label)
            return 0

        started = time.perf_counter()
        if not renderer.step_n(args.frames):
            raise SystemExit("realtime frame stepping failed")
        if args.inspect_framebuffer:
            inspect_framebuffer(renderer)
        saved = save_screenshot(renderer, args.out, launch_cwd=launch_cwd)
        print(
            f"[caustica] Saved {saved} after {args.frames} frame(s) "
            f"in {time.perf_counter() - started:.2f}s"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

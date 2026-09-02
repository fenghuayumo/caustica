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
import math
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
    make_engine,
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
    parser.add_argument(
        "--camera-lut",
        type=Path,
        help="Apply a fitted 1D or 3D .cube camera LUT after tone mapping.",
    )
    parser.add_argument(
        "--tone-mapper",
        choices=["linear", "reinhard", "aces", "pbr-neutral", "soft-shoulder", "agx"],
        default="aces",
        help="Base tone mapper used before --camera-lut (default: aces).",
    )

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


def apply_scene_edits(engine, args: argparse.Namespace) -> None:
    if args.spawn:
        spawn_path = require_input_file(args.spawn, "Spawn asset")
        entity = engine.spawn_from_file(str(spawn_path))
        if entity is None:
            raise SystemExit(f"spawn_from_file failed: {spawn_path}")
        entity.translation = tuple(args.spawn_position)
        engine.step_n(1)
        print(f"[caustica] Spawned {entity.name}: {spawn_path}")
        if args.camera_pos is None:
            framing = scene_bounds_center_radius(engine)
            if framing is not None:
                frame_bounds(engine, *framing)

    for name, red, green, blue in args.material or []:
        scene = engine.scene
        material = None if scene is None else scene.find_material(name)
        if material is None:
            print(f"[caustica] WARNING: material not found: {name}")
            continue
        material.base_color = (float(red), float(green), float(blue))
        print(f"[caustica] Updated material: {name}")

    if bool(args.camera_pos) != bool(args.camera_dir):
        raise SystemExit("--camera-pos and --camera-dir must be provided together")
    if args.camera_pos and args.camera_dir:
        engine.set_camera_pos_dir_up(
            tuple(args.camera_pos), tuple(args.camera_dir), tuple(args.camera_up)
        )
    if args.fov is not None:
        engine.set_camera_vertical_fov(math.radians(args.fov))


def inspect_framebuffer(engine) -> None:
    framebuffer = engine.read_ldr_framebuffer()
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

    with make_engine(
        caustica,
        args,
        scene=resolve_scene_arg(args.scene),
        realtime=not is_reference,
        headless=not is_windowed,
        accumulation_target=args.spp if is_reference else 1,
    ) as engine:
        apply_common_settings(engine, caustica, bounces=args.bounces)
        tone_mapper = {
            "linear": caustica.ToneMapOperator.Linear,
            "reinhard": caustica.ToneMapOperator.Reinhard,
            "aces": caustica.ToneMapOperator.Aces,
            "pbr-neutral": caustica.ToneMapOperator.PbrNeutral,
            "soft-shoulder": caustica.ToneMapOperator.IdentitySoftShoulder,
            "agx": caustica.ToneMapOperator.AgX,
        }[args.tone_mapper]
        engine.settings.tone_mapping_params.tone_map_operator = tone_mapper
        if args.camera_lut:
            lut_path = require_input_file(args.camera_lut, "Camera LUT")
            tone_params = engine.settings.tone_mapping_params
            tone_params.auto_exposure = False
            tone_params.camera_lut_after_tone_map = True
            tone_params.load_camera_lut(str(lut_path))
            print(f"[caustica] Camera LUT: {lut_path}")
        mode_label = (
            apply_reference_mode(engine, caustica, spp=args.spp, oidn=args.oidn)
            if is_reference
            else apply_realtime_mode(engine, caustica, args.denoiser)
        )
        apply_scene_edits(engine, args)

        print(f"[caustica] Scene: {engine.scene_name}")
        print(f"[caustica] Mode : {mode_label}")

        if is_windowed:
            run_window_loop(engine)
            return 0

        if is_reference:
            if args.inspect_framebuffer:
                engine.step_until_accumulated()
                inspect_framebuffer(engine)
                save_screenshot(engine, args.out, launch_cwd=launch_cwd)
            else:
                render_reference_to(engine, args.out, launch_cwd=launch_cwd, label=mode_label)
            return 0

        started = time.perf_counter()
        if not engine.step_n(args.frames):
            raise SystemExit("realtime frame stepping failed")
        if args.inspect_framebuffer:
            inspect_framebuffer(engine)
        saved = save_screenshot(engine, args.out, launch_cwd=launch_cwd)
        print(
            f"[caustica] Saved {saved} after {args.frames} frame(s) "
            f"in {time.perf_counter() - started:.2f}s"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

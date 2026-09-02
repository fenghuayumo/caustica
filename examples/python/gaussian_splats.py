#!/usr/bin/env python
"""Render 3D Gaussian splats with caustica.

Three workflows share one script because they differ only in where the camera
and the splat data come from:

    view    Load a standalone .ply, frame it from its own bounds, and preview it
            interactively or render it headless.
    hybrid  Render a scene whose JSON already declares both meshes and 3DGS
            nodes, using raster splats with ray-traced soft shadows and
            emissive splat proxies.
    colmap  Reproduce COLMAP camera poses, including off-center pinhole
            intrinsics, and write one image per view.

Examples:
    python examples/python/gaussian_splats.py view --ply splat.ply
    python examples/python/gaussian_splats.py view --ply splat.ply --mode batch --out-dir out
    python examples/python/gaussian_splats.py hybrid --window
    python examples/python/gaussian_splats.py colmap --ply splat.ply --colmap-dir sparse
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

from _common import (
    ASSETS_DIR,
    add_device_args,
    add_quality_args,
    add_window_args,
    apply_common_settings,
    apply_realtime_mode,
    apply_reference_mode,
    import_caustica,
    make_engine,
    normalize,
    render_reference_to,
    require_input_file,
    resolve_output_path,
    resolve_scene_arg,
    run_window_loop,
    save_screenshot,
)
from _gaussian import (
    add_gaussian_args,
    add_gaussian_shadow_args,
    apply_gaussian_settings,
    camera_from_bounds,
    create_splat_only_scene,
    read_ply_bounds,
    rebuild_acceleration_structures,
)

DEFAULT_HYBRID_SCENE = ASSETS_DIR / "scenes" / "default" / "default.scene.json"

PLY_HINT = (
    "Pass --ply pointing at a binary little-endian 3DGS .ply file, for example a\n"
    "trained gsplat/Inria checkpoint."
)


# --------------------------------------------------------------------------
# view
# --------------------------------------------------------------------------


def aim_camera_at_ply(engine, args: argparse.Namespace, ply_path: Path) -> None:
    center, extents, vertex_count = read_ply_bounds(
        ply_path, convert_rdf_to_rub=args.rdf_to_rub, sample_cap=args.sample_cap
    )
    cam_pos, cam_dir, cam_up = camera_from_bounds(
        center, extents, args.side, args.distance_scale
    )
    if args.cam_pos:
        cam_pos = tuple(args.cam_pos)
    if args.cam_dir:
        cam_dir = normalize(args.cam_dir)
    if args.cam_up:
        cam_up = normalize(args.cam_up)

    print(f"[caustica] PLY vertices={vertex_count} center={center} extents={extents}")
    print(f"[caustica] camera pos={cam_pos} dir={cam_dir}")
    engine.set_camera_pos_dir_up(cam_pos, cam_dir, cam_up)
    engine.set_camera_vertical_fov(math.radians(args.fov))


def open_splat_engine(caustica, args, ply_path: Path, *, realtime: bool, headless: bool):
    """Create an EngineApp on the splat-only scene and append the .ply."""
    engine = make_engine(
        caustica,
        args,
        scene=args.scene or create_splat_only_scene(),
        realtime=realtime,
        headless=headless,
        accumulation_target=args.spp if not realtime else 1,
    )
    if not engine.load_gaussian_splat_file(str(ply_path), args.rdf_to_rub):
        engine.shutdown()
        raise SystemExit(f"Failed to load Gaussian splats: {ply_path}")
    return engine


def run_view(caustica, args: argparse.Namespace, launch_cwd: Path) -> int:
    ply_path = require_input_file(args.ply, "3DGS PLY", hint=PLY_HINT)
    print(f"[caustica] mode={args.mode} ply={ply_path}")

    if args.mode == "interactive":
        with open_splat_engine(
            caustica, args, ply_path, realtime=True, headless=False
        ) as engine:
            apply_realtime_mode(engine, caustica, "off")
            apply_common_settings(engine, caustica, bounces=args.bounces)
            apply_gaussian_settings(caustica, engine.settings, args)
            aim_camera_at_ply(engine, args, ply_path)
            run_window_loop(engine)
        return 0

    out_dir = resolve_output_path(args.out_dir, launch_cwd)
    batch = args.mode == "batch"

    if args.mode in {"reference", "batch"}:
        with open_splat_engine(
            caustica, args, ply_path, realtime=False, headless=True
        ) as engine:
            label = apply_reference_mode(engine, caustica, spp=args.spp, oidn=True)
            apply_common_settings(engine, caustica, bounces=args.bounces)
            apply_gaussian_settings(caustica, engine.settings, args)
            aim_camera_at_ply(engine, args, ply_path)
            engine.settings.reset_accumulation = True
            out = out_dir / "reference_oidn.png" if batch else args.out
            render_reference_to(engine, out, launch_cwd=launch_cwd, label=label)

    if args.mode in {"realtime", "batch"}:
        with open_splat_engine(
            caustica, args, ply_path, realtime=True, headless=True
        ) as engine:
            label = apply_realtime_mode(engine, caustica, args.denoiser)
            apply_common_settings(engine, caustica, bounces=args.bounces)
            apply_gaussian_settings(caustica, engine.settings, args)
            aim_camera_at_ply(engine, args, ply_path)
            engine.settings.reset_accumulation = True
            engine.step_n(args.frames)
            out = out_dir / "realtime.png" if batch else args.out
            saved = save_screenshot(engine, out, launch_cwd=launch_cwd)
            print(f"[caustica] Saved {saved} after {args.frames} frame(s) [{label}]")

    return 0


# --------------------------------------------------------------------------
# hybrid
# --------------------------------------------------------------------------


def run_hybrid(caustica, args: argparse.Namespace, launch_cwd: Path) -> int:
    scene = resolve_scene_arg(args.scene)
    print(f"[caustica] Scene : {scene}")
    print(f"[caustica] Mode  : {'headless' if args.headless else 'windowed'}")

    with make_engine(
        caustica,
        args,
        scene=scene,
        realtime=not args.headless,
        accumulation_target=args.spp,
    ) as engine:
        print(f"[caustica] Loaded scene   : {engine.scene_name}")
        print(f"[caustica] 3DGS objects   : {engine.gaussian_splat_object_count}")
        print(f"[caustica] 3DGS splats    : {engine.gaussian_splat_count}")
        if engine.gaussian_splat_object_count == 0:
            print(
                "[caustica] WARNING: this scene declares no GaussianSplat nodes, so the "
                "3DGS shadow and emission settings will have no visible effect.\n"
                "[caustica]          Point --scene at a scene containing 3DGS nodes, or use "
                "the 'view' subcommand to render a standalone .ply."
            )

        if args.headless:
            label = apply_reference_mode(engine, caustica, spp=args.spp, oidn=args.oidn)
        else:
            label = apply_realtime_mode(engine, caustica, args.denoiser)
        apply_common_settings(engine, caustica, bounces=args.bounces)
        apply_gaussian_settings(caustica, engine.settings, args)
        engine.settings.reset_accumulation = True

        if args.shadow_mode != "disabled":
            rebuild_acceleration_structures(engine, args.warmup_frames)

        if args.headless:
            render_reference_to(engine, args.out, launch_cwd=launch_cwd, label=label)
        else:
            run_window_loop(engine)
    return 0


# --------------------------------------------------------------------------
# colmap
# --------------------------------------------------------------------------


def run_colmap(caustica, args: argparse.Namespace, launch_cwd: Path) -> int:
    import _colmap

    ply_path = require_input_file(args.ply, "3DGS PLY", hint=PLY_HINT)
    colmap_dir = Path(args.colmap_dir).expanduser().resolve()
    if not colmap_dir.is_dir():
        raise SystemExit(
            f"COLMAP model directory not found: {colmap_dir}\n"
            "Pass --colmap-dir pointing at a sparse model containing "
            "cameras.bin/images.bin (or the .txt equivalents)."
        )

    views = _colmap.load_views(colmap_dir, args.name_prefix, args.name_contains)[args.skip :]
    if args.max_views > 0:
        views = views[: args.max_views]
    if not views:
        raise SystemExit("No views left after --skip / --max-views")

    first = views[0]
    width = args.width or first.width
    height = args.height or first.height
    args.width, args.height = width, height

    out_dir = resolve_output_path(args.out_dir, launch_cwd)
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"[caustica] ply        : {ply_path}")
    print(f"[caustica] views      : {len(views)}")
    print(f"[caustica] resolution : {width}x{height}")
    print(f"[caustica] output     : {out_dir}")
    if args.symmetric_fov:
        print("[caustica] projection : symmetric vertical FOV (cx/cy ignored)")
    else:
        fx, fy, cx, cy = _colmap.scaled_intrinsics(first, width, height)
        print(
            f"[caustica] projection : COLMAP K scaled to output "
            f"(fx={fx:.3f}, fy={fy:.3f}, cx={cx:.3f}, cy={cy:.3f})"
        )

    records = []
    with open_splat_engine(
        caustica, args, ply_path, realtime=True, headless=not args.windowed
    ) as engine:
        apply_realtime_mode(engine, caustica, "off")
        settings = engine.settings
        settings.enable_tone_mapping = args.tonemap
        settings.enable_bloom = args.bloom
        apply_gaussian_settings(caustica, settings, args)

        if args.warmup_frames > 0:
            engine.step_n(args.warmup_frames)

        for index, view in enumerate(views):
            if view.width != first.width or view.height != first.height:
                print(
                    f"[warn] {view.name}: COLMAP size {view.width}x{view.height} "
                    f"differs from render size {width}x{height}"
                )

            record = apply_colmap_view(engine, _colmap, view, width, height, args)
            engine.step_n(max(1, args.frames_per_view))

            out_path = out_dir / f"{index:04d}_{_colmap.safe_stem(view.name)}.png"
            save_screenshot(engine, out_path)
            print(f"[caustica] saved {index + 1}/{len(views)}: {out_path.name}")
            records.append(
                {
                    "output": str(out_path),
                    "image_name": view.name,
                    "image_id": view.image_id,
                    "colmap_width": view.width,
                    "colmap_height": view.height,
                    "colmap_fx": view.fx,
                    "colmap_fy": view.fy,
                    "colmap_cx": view.cx,
                    "colmap_cy": view.cy,
                    **record,
                }
            )

    metadata_path = out_dir / "cameras_used.json"
    metadata_path.write_text(json.dumps(records, indent=2), encoding="utf-8")
    print(f"[caustica] wrote metadata: {metadata_path}")
    return 0


def apply_colmap_view(engine, _colmap, view, width: int, height: int, args) -> dict:
    cam_pos, cam_dir, cam_up = _colmap.caustica_camera(view, args.rdf_to_rub)
    engine.set_camera_pos_dir_up(cam_pos, cam_dir, cam_up)
    record: dict = {
        "caustica_position": cam_pos,
        "caustica_direction": cam_dir,
        "caustica_up": cam_up,
        "c2w": view.c2w.tolist(),
    }

    if args.symmetric_fov:
        engine.set_camera_vertical_fov(math.radians(view.vertical_fov_degrees))
        record["projection"] = "symmetric_vertical_fov"
        record["vertical_fov_degrees"] = view.vertical_fov_degrees
        return record

    fx, fy, cx, cy = _colmap.scaled_intrinsics(view, width, height)
    engine.set_camera_intrinsics(fx, fy, cx, cy, float(width), float(height))
    record.update(
        projection="pinhole_intrinsics", fx=fx, fy=fy, cx=cx, cy=cy, width=width, height=height
    )
    return record


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    # -- view ---------------------------------------------------------------
    view = subparsers.add_parser(
        "view", help="Preview or render a standalone 3DGS .ply file."
    )
    view.set_defaults(func=run_view)
    view.add_argument("--ply", required=True, help="Binary little-endian 3DGS .ply file.")
    view.add_argument(
        "--mode",
        choices=["interactive", "reference", "realtime", "batch"],
        default="interactive",
        help="batch renders reference and realtime back to back.",
    )
    view.add_argument(
        "--scene", default=None, help="Host scene JSON. Defaults to a splat-only scene."
    )
    view.add_argument("--out", default="splat.png", help="Output for reference/realtime mode.")
    view.add_argument("--out-dir", default="gaussian_splats_out", help="Output dir for batch mode.")
    view.add_argument("--frames", type=int, default=32, help="Realtime frames to accumulate.")
    view.add_argument("--denoiser", default="dlss-rr", help="Realtime denoiser / AA path.")
    view.add_argument(
        "--side", choices=["front", "back", "left", "right", "top"], default="front"
    )
    view.add_argument("--distance-scale", type=float, default=3.0)
    view.add_argument("--fov", type=float, default=45.0)
    view.add_argument("--cam-pos", nargs=3, type=float, metavar=("X", "Y", "Z"))
    view.add_argument("--cam-dir", nargs=3, type=float, metavar=("X", "Y", "Z"))
    view.add_argument("--cam-up", nargs=3, type=float, metavar=("X", "Y", "Z"))
    view.add_argument(
        "--sample-cap", type=int, default=200_000, help="Max PLY vertices sampled for bounds."
    )
    add_device_args(view)
    add_quality_args(view, spp=32)
    add_gaussian_args(view)
    add_rdf_arg(view)

    # -- hybrid -------------------------------------------------------------
    hybrid = subparsers.add_parser(
        "hybrid", help="Mesh + 3DGS scene with ray-traced soft shadows and emissive proxies."
    )
    hybrid.set_defaults(func=run_hybrid)
    hybrid.add_argument(
        "--scene",
        default=str(DEFAULT_HYBRID_SCENE),
        help="Scene JSON path or Assets-relative name. The scene must declare "
        "GaussianSplat nodes for this mode to show anything "
        "(default: Assets/scenes/default/default.scene.json).",
    )
    hybrid.add_argument("--out", default="hybrid_gaussian_scene.png")
    hybrid.add_argument("--denoiser", default="off", help="Realtime denoiser for --window.")
    hybrid.add_argument(
        "--oidn", action="store_true", help="Denoise the headless reference render with OIDN."
    )
    hybrid.add_argument(
        "--warmup-frames",
        type=int,
        default=8,
        help="Frames to run after enabling shadows so the AS rebuild completes.",
    )
    add_window_args(hybrid)
    add_device_args(hybrid)
    add_quality_args(hybrid, spp=32)
    add_gaussian_args(hybrid)
    add_gaussian_shadow_args(hybrid)
    add_rdf_arg(hybrid)

    # -- colmap -------------------------------------------------------------
    colmap = subparsers.add_parser(
        "colmap", help="Render a 3DGS .ply from COLMAP camera poses and intrinsics."
    )
    colmap.set_defaults(func=run_colmap)
    colmap.add_argument("--ply", required=True, help="Binary little-endian 3DGS .ply file.")
    colmap.add_argument(
        "--colmap-dir",
        required=True,
        help="COLMAP sparse model directory containing cameras/images (.bin or .txt).",
    )
    colmap.add_argument("--scene", default=None, help="Host scene JSON (default: splat-only).")
    colmap.add_argument("--out-dir", default="colmap_views_out")
    colmap.add_argument("--max-views", type=int, default=8, help="0 renders every view.")
    colmap.add_argument("--skip", type=int, default=0)
    colmap.add_argument("--name-prefix", default=None)
    colmap.add_argument("--name-contains", default=None)
    colmap.add_argument("--frames-per-view", type=int, default=8)
    colmap.add_argument("--warmup-frames", type=int, default=4)
    colmap.add_argument("--windowed", action="store_true")
    colmap.add_argument("--tonemap", action="store_true")
    colmap.add_argument("--bloom", action="store_true")
    colmap.add_argument(
        "--symmetric-fov",
        action="store_true",
        help="Ignore cx/cy and use a symmetric vertical FOV instead of the COLMAP K.",
    )
    # COLMAP output size comes from the model unless overridden.
    add_device_args(colmap, width=0, height=0)
    add_gaussian_args(colmap)
    add_rdf_arg(colmap)
    # COLMAP comparisons favour unquantized splats and no depth test.
    colmap.set_defaults(
        spp=1, bounces=8, storage_format="float32", mip_antialiasing=True, depth_test=False
    )

    return parser


def add_rdf_arg(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--rdf-to-rub",
        dest="rdf_to_rub",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Convert splat data from RDF (COLMAP/OpenCV) to RUB (caustica) axes.",
    )


def main() -> int:
    args = build_parser().parse_args()
    caustica = import_caustica()
    return args.func(caustica, args, Path.cwd())


if __name__ == "__main__":
    raise SystemExit(main())

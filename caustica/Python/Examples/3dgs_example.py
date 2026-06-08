#!/usr/bin/env python
"""Render a 3DGS PLY from Python in Reference and Realtime modes.

The default test file is:
    D:/ScanVideo/bieshu1/splat_30000.ply

Outputs:
    reference_oidn.png      Reference mode, 32 spp, OIDN at completion
    realtime_<aa>.png       Realtime mode, 32 stepped frames, DLSS/DLSS-RR if available
"""

from __future__ import annotations

import argparse
import time
from pathlib import Path

from test_splat_interactive import (
    camera_from_bounds,
    configure_import_path,
    create_splat_only_scene,
    normalize,
    parse_binary_ply_bounds,
)


DEFAULT_PLY = Path(r"D:/ScanVideo/bieshu1/splat_30000.ply")


def parse_vec3(values: list[float] | None) -> tuple[float, float, float] | None:
    if values is None:
        return None
    return (float(values[0]), float(values[1]), float(values[2]))


def configure_gaussian_splats(caustica, settings, args: argparse.Namespace) -> None:
    settings.enable_gaussian_splats = True
    settings.gaussian_splat_depth_test = args.depth_test
    settings.gaussian_splat_sorting_mode = int(
        caustica.GaussianSplatSortMode.StochasticSplats
        if args.sorting == "stochastic"
        else caustica.GaussianSplatSortMode.GpuSort
    )

    storage = {
        "float32": caustica.GaussianSplatStorageFormat.Float32,
        "float16": caustica.GaussianSplatStorageFormat.Float16,
        "uint8": caustica.GaussianSplatStorageFormat.Uint8,
    }[args.storage_format]
    settings.gaussian_splat_sh_format = int(storage)
    settings.gaussian_splat_rgba_format = int(storage)

    settings.gaussian_splat_scale = args.splat_scale
    settings.gaussian_splat_alpha_scale = args.alpha_scale
    settings.gaussian_splat_brightness = args.brightness
    settings.gaussian_splat_alpha_cull_threshold = args.alpha_cull
    settings.gaussian_splat_mip_antialiasing = args.mip_antialiasing
    settings.gaussian_splat_quantize_normals = args.quantize_normals
    settings.gaussian_splat_frustum_culling = int(
        {
            "disabled": caustica.GaussianSplatFrustumCulling.Disabled,
            "distance": caustica.GaussianSplatFrustumCulling.AtDistanceStage,
            "raster": caustica.GaussianSplatFrustumCulling.AtRasterStage,
        }[args.frustum_culling]
    )
    settings.gaussian_splat_frustum_dilation = args.frustum_dilation
    settings.gaussian_splat_screen_size_culling = args.screen_size_culling
    settings.gaussian_splat_min_pixel_coverage = args.min_pixel_coverage
    settings.gaussian_splat_ftb_sync_mode = int(
        caustica.GaussianSplatFTBSyncMode.Interlock
        if args.ftb_sync == "interlock"
        else caustica.GaussianSplatFTBSyncMode.Disabled
    )

    shadow_mode = {
        "disabled": caustica.GaussianSplatShadowMode.Disabled,
        "hard": caustica.GaussianSplatShadowMode.Hard,
        "soft": caustica.GaussianSplatShadowMode.Soft,
    }[args.shadow_mode]
    settings.gaussian_splat_shadows_mode = int(shadow_mode)
    settings.gaussian_splat_shadows = args.shadow_mode != "disabled"
    settings.gaussian_splat_shadow_strength = args.shadow_strength
    settings.gaussian_splat_shadow_soft_radius = args.shadow_soft_radius
    settings.gaussian_splat_shadow_soft_sample_count = args.shadow_soft_samples

    settings.gaussian_splat_use_aabbs = args.use_aabbs
    settings.gaussian_splat_use_tlas_instances = args.use_tlas_instances
    settings.gaussian_splat_blas_compaction = args.blas_compaction

    if args.translation is not None:
        settings.gaussian_splat_translation = parse_vec3(args.translation)
    if args.rotation is not None:
        settings.gaussian_splat_rotation_euler_deg = parse_vec3(args.rotation)
    if args.object_scale is not None:
        settings.gaussian_splat_object_scale = parse_vec3(args.object_scale)


def configure_camera(renderer, args: argparse.Namespace, ply_path: Path) -> None:
    center, extents, vertex_count = parse_binary_ply_bounds(
        ply_path, args.rdf_to_donut, args.sample_cap
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

    print(f"[caustica] PLY vertices : {vertex_count}")
    print(f"[caustica] PLY center   : {center}")
    print(f"[caustica] PLY extents  : {extents}")
    print(f"[caustica] camera pos   : {cam_pos}")
    print(f"[caustica] camera dir   : {cam_dir}")

    renderer.set_camera(cam_pos, cam_dir, cam_up)
    renderer.set_camera_fov(args.fov)


def make_renderer(caustica, args: argparse.Namespace, scene: str, ply_path: Path, realtime: bool):
    renderer = caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=True,
        vulkan=args.vulkan,
        adapter_index=args.adapter_index,
        scene=scene,
        realtime=realtime,
        accumulation_target=args.frames,
    )
    if not renderer.load_gaussian_splats(str(ply_path), args.rdf_to_donut):
        renderer.close()
        raise RuntimeError(f"Failed to load Gaussian splat: {ply_path}")
    return renderer


def save_or_raise(renderer, path: Path) -> None:
    if not renderer.save_screenshot(str(path)):
        raise RuntimeError(f"Failed to save screenshot: {path}")
    print(f"[caustica] saved: {path}")


def render_reference_oidn(caustica, args: argparse.Namespace, scene: str, ply_path: Path, out_dir: Path) -> None:
    print(f"\n[caustica] Reference mode: accumulating {args.frames} spp with OIDN")
    renderer = make_renderer(caustica, args, scene, ply_path, realtime=False)
    try:
        settings = renderer.settings
        settings.path_tracer_mode = int(caustica.PathTracerMode.Reference)
        settings.accumulation_target = args.frames
        settings.accumulation_prewarm_realtime_caches = False
        settings.realtime_aa = int(caustica.RealtimeAA.Off)
        settings.enable_tone_mapping = args.tonemap
        settings.enable_bloom = args.bloom
        settings.oidn_enabled = True
        settings.oidn_use_gpu = args.oidn_gpu
        settings.oidn_quality = int(caustica.OidnQuality.High)
        settings.oidn_passes = int(caustica.OidnPasses.AlbedoNormal)
        settings.oidn_prefilter = int(caustica.OidnPrefilter.Accurate)
        settings.bounce_count = args.bounces
        settings.diffuse_bounce_count = args.diffuse_bounces
        settings.use_nee = True
        settings.oidn_apply()
        configure_gaussian_splats(caustica, settings, args)
        configure_camera(renderer, args, ply_path)
        settings.reset_accumulation = True

        start = time.time()
        frames_done = renderer.step_until_accumulated(max(args.frames + 128, args.frames * 4))
        print(f"[caustica] reference frames executed: {frames_done} ({time.time() - start:.2f}s)")
        save_or_raise(renderer, out_dir / "reference_oidn.png")
    finally:
        renderer.close()


def select_realtime_aa(caustica, settings, requested: str) -> tuple[int, str]:
    dlss_supported = bool(getattr(settings, "is_dlss_supported", False))
    rr_supported = bool(getattr(settings, "is_dlss_rr_supported", False))

    if requested == "dlss-rr":
        if rr_supported:
            return int(caustica.RealtimeAA.DLSS_RR), "dlss_rr"
        if dlss_supported:
            print("[caustica] DLSS-RR unavailable; falling back to DLSS.")
            return int(caustica.RealtimeAA.DLSS), "dlss"
        print("[caustica] DLSS/DLSS-RR unavailable; falling back to TAA.")
        return int(caustica.RealtimeAA.TAA), "taa"

    if requested == "dlss":
        if dlss_supported:
            return int(caustica.RealtimeAA.DLSS), "dlss"
        print("[caustica] DLSS unavailable; falling back to TAA.")
        return int(caustica.RealtimeAA.TAA), "taa"

    if requested == "taa":
        return int(caustica.RealtimeAA.TAA), "taa"
    return int(caustica.RealtimeAA.Off), "off"


def render_realtime(caustica, args: argparse.Namespace, scene: str, ply_path: Path, out_dir: Path) -> None:
    print(f"\n[caustica] Realtime mode: stepping {args.frames} frames with DLSS/DLSS-RR when available")
    renderer = make_renderer(caustica, args, scene, ply_path, realtime=True)
    try:
        settings = renderer.settings
        settings.path_tracer_mode = int(caustica.PathTracerMode.Realtime)
        aa_mode, aa_label = select_realtime_aa(caustica, settings, args.realtime_aa)
        settings.realtime_aa = aa_mode
        settings.standalone_denoiser = False
        settings.enable_tone_mapping = args.tonemap
        settings.enable_bloom = args.bloom
        settings.accumulation_target = args.frames
        settings.accumulation_prewarm_realtime_caches = False
        configure_gaussian_splats(caustica, settings, args)
        configure_camera(renderer, args, ply_path)

        if hasattr(settings, "dlss_mode") and aa_label in {"dlss", "dlss_rr"}:
            settings.dlss_mode = int(caustica.DLSSMode.Balanced)
        if hasattr(settings, "dlss_rr_preset") and aa_label == "dlss_rr":
            settings.dlss_rr_preset = int(caustica.DLSSRRPreset.PresetE)
            settings.disable_restirs_with_dlss_rr = True
        if hasattr(settings, "dlss_fg_mode"):
            settings.dlss_fg_mode = int(caustica.DLSSFGMode.Off)

        settings.reset_accumulation = True
        start = time.time()
        renderer.step_n(args.frames)
        print(f"[caustica] realtime AA: {aa_label}; frames executed: {args.frames} ({time.time() - start:.2f}s)")
        save_or_raise(renderer, out_dir / f"realtime_{aa_label}.png")
    finally:
        renderer.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="caustica 3DGS Reference/OIDN and Realtime/DLSS example.")
    parser.add_argument("--ply", type=Path, default=DEFAULT_PLY, help="Path to the 3DGS .ply file.")
    parser.add_argument("--scene", default=None, help="Optional scene file. Defaults to a hidden-dummy scene.")
    parser.add_argument("--out-dir", type=Path, default=Path("3dgs_example_out"))
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--frames", type=int, default=32, help="Reference spp and realtime frame count.")
    parser.add_argument("--bounces", type=int, default=8)
    parser.add_argument("--diffuse-bounces", type=int, default=3)
    parser.add_argument("--vulkan", action="store_true")
    parser.add_argument("--adapter-index", type=int, default=-1)

    parser.add_argument("--side", choices=["front", "back", "left", "right", "top"], default="front")
    parser.add_argument("--distance-scale", type=float, default=3.0)
    parser.add_argument("--fov", type=float, default=45.0)
    parser.add_argument("--cam-pos", nargs=3, type=float, metavar=("X", "Y", "Z"))
    parser.add_argument("--cam-dir", nargs=3, type=float, metavar=("X", "Y", "Z"))
    parser.add_argument("--cam-up", nargs=3, type=float, metavar=("X", "Y", "Z"))
    parser.add_argument("--sample-cap", type=int, default=200_000)

    parser.add_argument("--sorting", choices=["gpu", "stochastic"], default="gpu")
    parser.add_argument("--storage-format", choices=["float32", "float16", "uint8"], default="uint8")
    parser.add_argument("--splat-scale", type=float, default=1.0)
    parser.add_argument("--alpha-scale", type=float, default=1.0)
    parser.add_argument("--brightness", type=float, default=1.0)
    parser.add_argument("--alpha-cull", type=float, default=1.0 / 255.0)
    parser.add_argument("--translation", nargs=3, type=float, metavar=("X", "Y", "Z"))
    parser.add_argument("--rotation", nargs=3, type=float, metavar=("X", "Y", "Z"))
    parser.add_argument("--object-scale", nargs=3, type=float, metavar=("X", "Y", "Z"))
    parser.add_argument("--depth-test", dest="depth_test", action="store_true", default=True)
    parser.add_argument("--no-depth-test", dest="depth_test", action="store_false")
    parser.add_argument("--rdf-to-donut", dest="rdf_to_donut", action="store_true", default=True)
    parser.add_argument("--no-rdf-to-donut", dest="rdf_to_donut", action="store_false")

    parser.add_argument("--mip-antialiasing", action="store_true")
    parser.add_argument("--quantize-normals", dest="quantize_normals", action="store_true", default=True)
    parser.add_argument("--no-quantize-normals", dest="quantize_normals", action="store_false")
    parser.add_argument("--frustum-culling", choices=["disabled", "distance", "raster"], default="raster")
    parser.add_argument("--frustum-dilation", type=float, default=0.20)
    parser.add_argument("--screen-size-culling", action="store_true")
    parser.add_argument("--min-pixel-coverage", type=float, default=1.0)
    parser.add_argument("--ftb-sync", choices=["disabled", "interlock"], default="disabled")

    parser.add_argument("--shadow-mode", choices=["disabled", "hard", "soft"], default="disabled")
    parser.add_argument("--shadow-strength", type=float, default=0.75)
    parser.add_argument("--shadow-soft-radius", type=float, default=0.08)
    parser.add_argument("--shadow-soft-samples", type=int, default=1)
    parser.add_argument("--use-aabbs", action="store_true")
    parser.add_argument("--use-tlas-instances", dest="use_tlas_instances", action="store_true", default=True)
    parser.add_argument("--no-tlas-instances", dest="use_tlas_instances", action="store_false")
    parser.add_argument("--blas-compaction", dest="blas_compaction", action="store_true", default=True)
    parser.add_argument("--no-blas-compaction", dest="blas_compaction", action="store_false")

    parser.add_argument("--realtime-aa", choices=["dlss-rr", "dlss", "taa", "off"], default="dlss-rr")
    parser.add_argument("--oidn-gpu", dest="oidn_gpu", action="store_true", default=True)
    parser.add_argument("--oidn-cpu", dest="oidn_gpu", action="store_false")
    parser.add_argument("--tonemap", action="store_true")
    parser.add_argument("--bloom", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    ply_path = args.ply.resolve()
    out_dir = args.out_dir.resolve()

    if not ply_path.exists():
        raise FileNotFoundError(f"PLY not found: {ply_path}")
    out_dir.mkdir(parents=True, exist_ok=True)

    configure_import_path()
    import caustica

    scene = args.scene or str(create_splat_only_scene())
    print(f"[caustica] PLY     : {ply_path}")
    print(f"[caustica] scene   : {scene}")
    print(f"[caustica] out dir : {out_dir}")
    print(f"[caustica] frames  : {args.frames}")

    render_reference_oidn(caustica, args, scene, ply_path, out_dir)
    render_realtime(caustica, args, scene, ply_path, out_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

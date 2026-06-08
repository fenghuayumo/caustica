#!/usr/bin/env python
"""Render Assets/default.json with 3DGS soft shadows and emitter lighting.

The default scene places Antman (OBJ), Gingy (3DGS PLY), and a ground plane
under directional + point lights. This script uses Hybrid 3DGS + 3DGRT mode
(raster splats + RTX particle shadows) with soft shadows, and treats the splat
radiance field as emissive proxy lights so mesh geometry receives illumination
from the 3DGS object.

Usage:
    cd <repo>
    python caustica/Python/Examples/render_default_scene.py

    # Headless reference render (default):
    python caustica/Python/Examples/render_default_scene.py --headless --out default_scene_3dgs.png

    # Interactive preview window:
    python caustica/Python/Examples/render_default_scene.py --window
    python caustica/Python/Examples/render_default_scene.py --no-headless
"""

from __future__ import annotations

import argparse
import glob
import os
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_SCENE = REPO_ROOT / "Assets" / "default.json"


def configure_import_path() -> None:
    try:
        import caustica  # noqa: F401

        return
    except ImportError:
        pass

    candidates = [
        REPO_ROOT / "bin",
        REPO_ROOT / "build-linux" / "bin",
        REPO_ROOT / "build" / "caustica" / "Release",
        Path(__file__).resolve().parent,
    ]
    for candidate in candidates:
        if glob.glob(str(candidate / "caustica*.pyd")) or glob.glob(str(candidate / "caustica*.so")):
            sys.path.insert(0, str(candidate))
            os.environ["PATH"] = str(candidate) + os.pathsep + os.environ.get("PATH", "")
            os.chdir(candidate)
            return

    searched = "\n".join(f"  {p}" for p in candidates)
    raise RuntimeError(f"Could not find caustica Python module. Searched:\n{searched}")


def configure_gaussian_splats(caustica, settings, args: argparse.Namespace) -> None:
    settings.enable_gaussian_splats = True
    settings.gaussian_splat_depth_test = args.depth_test
    settings.gaussian_splat_scale = args.splat_scale
    settings.gaussian_splat_alpha_scale = args.alpha_scale
    settings.gaussian_splat_brightness = args.brightness
    settings.gaussian_splat_alpha_cull_threshold = args.alpha_cull

    if args.render_mode == "hybrid":
        # UI "Hybrid 3DGS + 3DGRT": raster overlay + RTX particle shadow AS.
        settings.gaussian_splat_shadows = True
        settings.gaussian_splat_hybrid_shadows = True
        settings.gaussian_splat_shadows_mode = int(caustica.GaussianSplatShadowMode.Soft)
        settings.gaussian_splat_shadow_strength = args.shadow_strength
        settings.gaussian_splat_shadow_soft_radius = args.shadow_soft_radius
        settings.gaussian_splat_shadow_soft_sample_count = args.shadow_soft_samples
        settings.gaussian_splat_use_tlas_instances = True
        settings.gaussian_splat_blas_compaction = True
        settings.gaussian_splat_rtx_kernel_degree = args.rtx_kernel_degree
        settings.gaussian_splat_rtx_adaptive_clamp = args.rtx_adaptive_clamp
        settings.gaussian_splat_rtx_particle_shadow_offset = args.rtx_shadow_offset
    else:
        # UI "Raster 3DGS (VS)": splats only, no RTX shadow AS.
        settings.gaussian_splat_shadows = False
        settings.gaussian_splat_hybrid_shadows = False
        settings.gaussian_splat_shadows_mode = int(caustica.GaussianSplatShadowMode.Disabled)

    settings.gaussian_splat_as_emitter = True
    settings.gaussian_splat_emission_intensity = args.emission_intensity
    settings.gaussian_splat_emission_max_proxy_count = args.emission_max_proxies


def apply_gaussian_settings_and_rebuild(renderer, caustica, settings, args: argparse.Namespace) -> None:
    configure_gaussian_splats(caustica, settings, args)
    settings.reset_accumulation = True
    if args.render_mode == "hybrid":
        renderer.app.request_accel_rebuild()
        warmup = max(args.warmup_frames, 1)
        print(f"[caustica] Rebuilding 3DGRT acceleration structures ({warmup} warmup frames) ...")
        renderer.step_n(warmup)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Render Assets/default.json with 3DGS soft shadow and emitter lighting."
    )
    parser.add_argument(
        "--scene",
        default=str(DEFAULT_SCENE),
        help="Scene JSON path or Assets-relative name (default: Assets/default.json).",
    )
    mode_group = parser.add_mutually_exclusive_group()
    mode_group.add_argument("--headless", action="store_true", default=True,
                            help="Render offscreen and save a screenshot (default).")
    mode_group.add_argument("--window", "--no-headless", dest="headless", action="store_false",
                            help="Open an interactive preview window.")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--spp", type=int, default=32, help="Reference samples for headless mode.")
    parser.add_argument("--bounces", type=int, default=8)
    parser.add_argument("--out", default="default_scene_3dgs.png",
                        help="Screenshot path for headless mode.")
    parser.add_argument("--vulkan", action="store_true", help="Use Vulkan backend.")
    parser.add_argument("--oidn", action="store_true",
                        help="Enable OIDN denoising after reference accumulation.")
    parser.add_argument("--oidn-gpu", dest="oidn_gpu",
                        action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--oidn-quality", type=int, default=2,
                        help="OIDN quality: 0=Fast, 1=Balanced, 2=High.")

    parser.add_argument("--depth-test", dest="depth_test", action="store_true", default=True)
    parser.add_argument("--no-depth-test", dest="depth_test", action="store_false")
    parser.add_argument("--render-mode", choices=["hybrid", "raster"], default="hybrid",
                        help="3DGS render mode: hybrid (3DGS + 3DGRT shadows) or raster-only.")
    parser.add_argument("--warmup-frames", type=int, default=8,
                        help="Frames to run after enabling hybrid mode so RTX AS rebuild completes.")
    parser.add_argument("--splat-scale", type=float, default=1.0)
    parser.add_argument("--alpha-scale", type=float, default=1.0)
    parser.add_argument("--brightness", type=float, default=1.0)
    parser.add_argument("--alpha-cull", type=float, default=1.0 / 255.0)
    parser.add_argument("--shadow-strength", type=float, default=0.75)
    parser.add_argument("--shadow-soft-radius", type=float, default=0.08)
    parser.add_argument("--shadow-soft-samples", type=int, default=1)
    parser.add_argument("--rtx-kernel-degree", type=int, default=0,
                        help="3DGRT particle kernel degree (0=Linear .. 5=Quintic).")
    parser.add_argument("--rtx-adaptive-clamp", dest="rtx_adaptive_clamp",
                        action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--rtx-shadow-offset", type=float, default=0.01,
                        help="3DGRT particle shadow ray offset.")
    parser.add_argument("--emission-intensity", type=float, default=1.0,
                        help="3DGS emissive proxy intensity multiplier.")
    parser.add_argument("--emission-max-proxies", type=int, default=8192,
                        help="Maximum number of 3DGS emissive proxy lights.")
    return parser


def parse_args() -> argparse.Namespace:
    return build_arg_parser().parse_args()


def resolve_scene_arg(scene_arg: str) -> str:
    path = Path(scene_arg)
    if path.is_file():
        return str(path.resolve())
    assets_candidate = REPO_ROOT / "Assets" / scene_arg
    if assets_candidate.is_file():
        return str(assets_candidate.resolve())
    return scene_arg


def main() -> int:
    args = parse_args()
    if args.oidn and not args.headless:
        print("[caustica] --oidn uses reference accumulation; enabling --headless")
        args.headless = True

    launch_cwd = Path.cwd()
    configure_import_path()
    import caustica

    scene = resolve_scene_arg(args.scene)
    mode = "headless" if args.headless else "windowed"
    print(f"[caustica] Scene : {scene}")
    print(f"[caustica] Mode  : {mode}")
    render_label = "Hybrid 3DGS + 3DGRT" if args.render_mode == "hybrid" else "Raster 3DGS (VS)"
    print(f"[caustica] 3DGS  : {render_label}, soft shadow + emitter lighting")

    use_reference = args.headless or args.oidn
    renderer = caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=args.headless,
        vulkan=args.vulkan,
        scene=scene,
        realtime=not use_reference,
        accumulation_target=args.spp,
    )

    try:
        print(f"[caustica] Loaded scene: {renderer.app.scene_name}")
        scene_obj = renderer.app.scene
        print(f"[caustica] Materials in scene: {scene_obj.material_count}")
        print(f"[caustica] Lights in scene   : {scene_obj.light_count}")
        print(f"[caustica] 3DGS objects      : {renderer.app.gaussian_splat_object_count}")
        print(f"[caustica] 3DGS splat count  : {renderer.app.gaussian_splat_count}")

        settings = renderer.settings
        settings.realtime_mode = not use_reference
        settings.accumulation_target = args.spp
        settings.accumulation_prewarm_realtime_caches = False
        settings.bounce_count = args.bounces
        settings.use_nee = True
        settings.enable_tone_mapping = True
        settings.realtime_aa = int(caustica.RealtimeAA.Off)
        apply_gaussian_settings_and_rebuild(renderer, caustica, settings, args)

        if args.oidn:
            settings.oidn_enabled = True
            settings.oidn_use_gpu = args.oidn_gpu
            settings.oidn_quality = args.oidn_quality
            settings.oidn_apply()
            print("[caustica] OIDN enabled")

        if args.headless:
            label = f"{args.spp} spp"
            if args.oidn:
                label += " + OIDN"
            print(f"[caustica] Rendering {label} ...")
            t_start = time.time()
            frames = renderer.step_until_accumulated()
            elapsed = time.time() - t_start
            print(f"[caustica] Done in {elapsed:.2f}s ({frames} frames)")

            out_path = Path(args.out)
            if not out_path.is_absolute():
                out_path = launch_cwd / out_path
            out_path = out_path.resolve()
            if not renderer.save_screenshot(str(out_path)):
                raise RuntimeError(f"Failed to save screenshot: {out_path}")
            print(f"[caustica] Saved: {out_path}")
        else:
            print("[caustica] Ready. Close window or Ctrl+C to exit.")
            print("[caustica]   Left-click  -> Inspector (Transform)")
            print("[caustica]   Right-click -> Material Editor")
            while renderer.step(-1.0):
                time.sleep(0.001)
    except KeyboardInterrupt:
        print("\n[caustica] Interrupted.")
    finally:
        renderer.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

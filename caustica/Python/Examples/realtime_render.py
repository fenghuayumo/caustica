#!/usr/bin/env python
"""
Realtime rendering smoke test for the caustica Python extension.

Usage:
    python caustica\\Python\\Examples\\realtime_render.py ^
        --scene D:\\ProgramCode\\C++\\caustica\\Assets\\bistro-programmer-art.scene.json ^
        --denoiser nrd ^
        --width 1280 --height 720 --frames 32 ^
        --out realtime_frame.png

Unlike offline_render.py, this script normally keeps caustica in realtime mode and
advances the renderer with step()/step_n() instead of waiting for reference
accumulation. The --denoiser option exposes the realtime denoiser paths that
are currently available from Python.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
DEFAULT_BISTRO_SCENE = REPO_ROOT / "Assets" / "bistro-programmer-art.scene.json"


def configure_import_path() -> None:
    """Prefer a local build when this example is run directly from the repo."""
    bin_dir = REPO_ROOT / "bin"
    python_dir = REPO_ROOT / "python"
    if python_dir.exists():
        sys.path.insert(0, str(python_dir))
    if bin_dir.exists():
        sys.path.insert(0, str(bin_dir))


def default_scene() -> str:
    if DEFAULT_BISTRO_SCENE.exists():
        return str(DEFAULT_BISTRO_SCENE)
    return "default.json"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="caustica realtime headless render test.")
    parser.add_argument(
        "--scene",
        default=default_scene(),
        help="Scene path/name. Defaults to repo Assets/bistro-programmer-art.scene.json when present.",
    )
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--frames", type=int, default=32, help="Realtime frames to render before saving.")
    parser.add_argument("--out", default="realtime_frame.png", help="Output image path.")
    parser.add_argument("--vulkan", action="store_true", help="Use Vulkan instead of DX12.")
    parser.add_argument("--adapter-index", type=int, default=-1)
    parser.add_argument(
        "--denoiser",
        choices=["off", "taa", "nrd", "dlss", "oidn"],
        default="nrd",
        help=(
            "Realtime path to exercise. taa and nrd work headless. "
            "dlss means DLSS Ray Reconstruction when available; native NGX works headless, "
            "while Streamline DLSS requires a non-headless window. "
            "oidn is reference-mode OIDN because current caustica OIDN is not a realtime denoiser."
        ),
    )
    parser.add_argument(
        "--no-headless",
        dest="headless",
        action="store_false",
        default=True,
        help="Create a visible window while running the realtime test.",
    )
    parser.add_argument("--bounces", type=int, default=3)
    parser.add_argument(
        "--reference-spp",
        type=int,
        default=16,
        help="SPP target used only with --denoiser oidn.",
    )
    parser.add_argument("--oidn-gpu", dest="oidn_gpu", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--oidn-quality", type=int, default=2, help="OIDN quality: 0=Fast, 1=Balanced, 2=High.")
    parser.add_argument("--oidn-passes", type=int, default=2, help="OIDN guides: 0=ColorOnly, 1=Albedo, 2=AlbedoNormal.")
    parser.add_argument("--oidn-prefilter", type=int, default=2, help="OIDN prefilter: 0=None, 1=Fast, 2=Accurate.")
    parser.add_argument(
        "--restir-di",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Enable ReSTIR DI in realtime modes. Use --no-restir-di to disable it.",
    )
    parser.add_argument("--camera-pos", type=float, nargs=3, metavar=("X", "Y", "Z"))
    parser.add_argument("--camera-dir", type=float, nargs=3, metavar=("X", "Y", "Z"))
    parser.add_argument(
        "--camera-up",
        type=float,
        nargs=3,
        metavar=("X", "Y", "Z"),
        default=(0.0, 1.0, 0.0),
    )
    parser.add_argument("--fov", type=float, default=None, help="Vertical FOV in degrees.")
    return parser.parse_args()


def select_dlss_aa(caustica, settings) -> tuple[int, str]:
    if getattr(settings, "is_dlss_rr_supported", False):
        return int(caustica.RealtimeAA.DLSS_RR), "DLSS-RR"
    if getattr(settings, "is_dlss_supported", False):
        print("[caustica] DLSS-RR unavailable; falling back to DLSS Super Resolution.")
        return int(caustica.RealtimeAA.DLSS), "DLSS"
    print("[caustica] DLSS unavailable; falling back to TAA.")
    return int(caustica.RealtimeAA.TAA), "TAA"


def configure_mode(renderer, caustica, args: argparse.Namespace) -> tuple[str, bool]:
    settings = renderer.settings
    settings.accumulation_prewarm_realtime_caches = False
    settings.bounce_count = args.bounces
    settings.use_nee = True
    settings.enable_tone_mapping = True
    settings.use_restir_di = args.restir_di
    settings.use_restir_gi = True

    denoiser = args.denoiser
    if denoiser == "oidn":
        print("[caustica] NOTE: OIDN is implemented in caustica as a reference accumulation denoiser, not realtime.")
        renderer.app.set_reference_mode(
            spp=max(args.frames, 1),
            oidn=True,
            oidn_quality=args.oidn_quality,
            oidn_passes=args.oidn_passes,
            oidn_prefilter=args.oidn_prefilter,
        )
        settings.oidn_use_gpu = args.oidn_gpu
        settings.oidn_apply()
        settings.bounce_count = args.bounces
        return "OIDN reference", False

    renderer.app.set_realtime_mode(
        standalone_denoiser=(denoiser == "nrd"),
        realtime_aa=int(caustica.RealtimeAA.Off),
    )
    settings.accumulation_target = 1
    settings.reset_accumulation = True

    if denoiser == "taa":
        settings.realtime_aa = int(caustica.RealtimeAA.TAA)
        settings.standalone_denoiser = False
        return "TAA realtime", True

    if denoiser == "nrd":
        settings.realtime_aa = int(caustica.RealtimeAA.TAA)
        settings.standalone_denoiser = True
        return "NRD + TAA realtime", True

    if denoiser == "dlss":
        aa_mode, label = select_dlss_aa(caustica, settings)
        if args.headless and label in {"DLSS", "DLSS-RR"}:
            print("[caustica] DLSS/DLSS-RR is available in headless mode on native NGX builds.")
            print("[caustica] Streamline builds still need --no-headless for the Streamline DLSS path.")
        settings.realtime_aa = aa_mode
        settings.standalone_denoiser = False
        if hasattr(settings, "dlss_mode") and label in {"DLSS", "DLSS-RR"}:
            settings.dlss_mode = int(caustica.DLSSMode.Balanced)
        if hasattr(settings, "dlss_rr_preset") and label == "DLSS-RR":
            settings.dlss_rr_preset = int(caustica.DLSSRRPreset.PresetE)
            settings.dlss_rr_micro_jitter = 0.1
            settings.disable_restirs_with_dlss_rr = True
        if hasattr(settings, "dlss_fg_mode"):
            settings.dlss_fg_mode = int(caustica.DLSSFGMode.Off)
        return f"{label} realtime", True

    settings.realtime_aa = int(caustica.RealtimeAA.Off)
    settings.standalone_denoiser = False
    return "No denoiser realtime", True


def main() -> int:
    args = parse_args()
    configure_import_path()

    try:
        import caustica
    except ImportError as exc:
        sys.stderr.write(
            "Failed to import caustica. Build/install the Python package first, or run from a repo with bin/ present.\n"
        )
        raise exc

    print(f"[caustica] Mode: {caustica.MODE}")
    print(f"[caustica] Creating realtime Renderer ({args.width}x{args.height}, headless={args.headless}) ...")

    renderer = caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=args.headless,
        vulkan=args.vulkan,
        adapter_index=args.adapter_index,
        scene=args.scene,
        realtime=True,
        accumulation_target=1,
    )

    try:
        print(f"[caustica] Loaded scene: {renderer.app.scene_name}")
        scene = renderer.app.scene
        print(f"[caustica] Materials in scene: {scene.material_count}")
        print(f"[caustica] Lights in scene   : {scene.light_count}")

        mode_label, realtime_mode = configure_mode(renderer, caustica, args)

        if args.camera_pos and args.camera_dir:
            renderer.set_camera(tuple(args.camera_pos), tuple(args.camera_dir), tuple(args.camera_up))
        if args.fov is not None:
            renderer.set_camera_fov(args.fov)

        t_start = time.time()
        if realtime_mode:
            frames = max(args.frames, 1)
            print(f"[caustica] Rendering {frames} frames with {mode_label} ...")
            if not renderer.step_n(frames):
                raise RuntimeError("Realtime frame stepping failed.")
            frame_count = frames
        else:
            print(f"[caustica] Rendering reference accumulation with {mode_label} ({max(args.frames, 1)} spp) ...")
            frame_count = renderer.step_until_accumulated(max(args.frames + 128, args.frames * 4))
        elapsed = time.time() - t_start
        print(f"[caustica] Done in {elapsed:.2f}s ({frame_count} frames executed)")

        out_path = os.path.abspath(args.out)
        if not renderer.save_screenshot(out_path):
            raise RuntimeError(f"Failed to save screenshot to {out_path}")
        print(f"[caustica] Saved: {out_path}")
    finally:
        renderer.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

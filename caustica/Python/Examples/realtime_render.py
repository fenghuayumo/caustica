#!/usr/bin/env python
"""Realtime rendering smoke test for the caustica Python extension.

Usage:
    python caustica/Python/Examples/realtime_render.py \\
        --scene Assets/bistro-programmer-art.scene.json \\
        --denoiser nrd --frames 32 --out realtime_frame.png
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from _common import ASSETS_DIR, resolve_output_path, resolve_scene_arg


def default_scene() -> str:
    bistro = ASSETS_DIR / "bistro-programmer-art.scene.json"
    if bistro.exists():
        return str(bistro)
    return "default.json"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="caustica realtime render smoke test.")
    parser.add_argument("--scene", default=default_scene())
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--frames", type=int, default=32)
    parser.add_argument("--out", default="realtime_frame.png")
    parser.add_argument("--vulkan", action="store_true")
    parser.add_argument("--adapter-index", type=int, default=-1)
    parser.add_argument(
        "--denoiser",
        choices=["off", "taa", "nrd", "dlss", "oidn"],
        default="nrd",
        help="Realtime path (oidn switches to reference accumulation).",
    )
    parser.add_argument("--no-headless", dest="headless", action="store_false", default=True)
    parser.add_argument("--bounces", type=int, default=3)
    parser.add_argument("--restir-di", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--oidn-gpu", dest="oidn_gpu", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--oidn-quality", type=int, default=2)
    parser.add_argument("--oidn-passes", type=int, default=2)
    parser.add_argument("--oidn-prefilter", type=int, default=2)
    parser.add_argument("--camera-pos", type=float, nargs=3)
    parser.add_argument("--camera-dir", type=float, nargs=3)
    parser.add_argument("--camera-up", type=float, nargs=3, default=(0.0, 1.0, 0.0))
    parser.add_argument("--fov", type=float, default=None)
    return parser.parse_args()


def select_dlss_aa(caustica, settings) -> tuple[int, str]:
    if getattr(settings, "is_dlss_rr_supported", False):
        return int(caustica.RealtimeAA.DLSS_RR), "DLSS-RR"
    if getattr(settings, "is_dlss_supported", False):
        print("[caustica] DLSS-RR unavailable; falling back to DLSS.")
        return int(caustica.RealtimeAA.DLSS), "DLSS"
    print("[caustica] DLSS unavailable; falling back to TAA.")
    return int(caustica.RealtimeAA.TAA), "TAA"


def configure_mode(renderer, caustica, args: argparse.Namespace) -> tuple[str, bool]:
    settings = renderer.settings
    settings.bounce_count = args.bounces
    settings.use_nee = True
    settings.enable_tone_mapping = True
    settings.use_restir_di = args.restir_di
    settings.use_restir_gi = True

    if args.denoiser == "oidn":
        print("[caustica] NOTE: OIDN is a reference-mode denoiser in caustica.")
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
        standalone_denoiser=(args.denoiser == "nrd"),
        realtime_aa=int(caustica.RealtimeAA.Off),
    )
    settings.accumulation_target = 1
    settings.reset_accumulation = True

    if args.denoiser == "taa":
        settings.realtime_aa = int(caustica.RealtimeAA.TAA)
        settings.standalone_denoiser = False
        return "TAA realtime", True
    if args.denoiser == "nrd":
        settings.realtime_aa = int(caustica.RealtimeAA.TAA)
        settings.standalone_denoiser = True
        return "NRD + TAA realtime", True
    if args.denoiser == "dlss":
        aa_mode, label = select_dlss_aa(caustica, settings)
        settings.realtime_aa = aa_mode
        settings.standalone_denoiser = False
        if label in {"DLSS", "DLSS-RR"}:
            settings.dlss_mode = int(caustica.DLSSMode.Balanced)
        if label == "DLSS-RR":
            settings.dlss_rr_preset = int(caustica.DLSSRRPreset.PresetE)
            settings.disable_restirs_with_dlss_rr = True
        settings.dlss_fg_mode = int(caustica.DLSSFGMode.Off)
        return f"{label} realtime", True

    settings.realtime_aa = int(caustica.RealtimeAA.Off)
    settings.standalone_denoiser = False
    return "No denoiser realtime", True


def main() -> int:
    args = parse_args()
    launch_cwd = Path.cwd()
    try:
        import caustica
    except ImportError as exc:
        sys.stderr.write("Failed to import caustica. Install with: python -m pip install .\n")
        raise exc

    scene = resolve_scene_arg(args.scene)
    print(f"[caustica] Mode: {caustica.MODE}")
    print(f"[caustica] Creating realtime Renderer ({args.width}x{args.height})")

    with caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=args.headless,
        vulkan=args.vulkan,
        adapter_index=args.adapter_index,
        scene=scene,
        realtime=True,
        accumulation_target=1,
    ) as renderer:
        print(f"[caustica] Loaded scene: {renderer.app.scene_name}")
        mode_label, realtime_mode = configure_mode(renderer, caustica, args)

        if args.camera_pos and args.camera_dir:
            renderer.set_camera(tuple(args.camera_pos), tuple(args.camera_dir), tuple(args.camera_up))
        if args.fov is not None:
            renderer.set_camera_fov(args.fov)

        t0 = time.time()
        if realtime_mode:
            frames = max(args.frames, 1)
            print(f"[caustica] Rendering {frames} frames with {mode_label} ...")
            if not renderer.step_n(frames):
                raise RuntimeError("Realtime frame stepping failed.")
            frame_count = frames
        else:
            print(f"[caustica] Rendering reference with {mode_label} ...")
            frame_count = renderer.step_until_accumulated(max(args.frames + 128, args.frames * 4))
        print(f"[caustica] Done in {time.time() - t0:.2f}s ({frame_count} frames)")

        out_path = resolve_output_path(args.out, launch_cwd)
        if not renderer.save_screenshot(str(out_path)):
            raise RuntimeError(f"Failed to save screenshot: {out_path}")
        print(f"[caustica] Saved: {out_path}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python
"""Render an animation sequence through the installed caustica wheel.

Reference mode freezes each animation time, accumulates SPP samples, runs OIDN,
then advances to the next output frame. Realtime mode renders exactly one engine
frame per output frame and keeps NRD or DLSS-RR temporal history alive.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from _common import resolve_scene_arg


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", default="Assets/kitchen.scene.json")
    parser.add_argument("--out-dir", type=Path, default=Path("sequence"))
    parser.add_argument("--mode", choices=["reference", "realtime"], default="reference")
    parser.add_argument("--denoiser", choices=["nrd", "dlss-rr"], default="nrd")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--frames", type=int, default=1)
    parser.add_argument("--fps", type=float, default=24.0)
    parser.add_argument("--start-time", type=float, default=0.0)
    parser.add_argument("--spp", type=int, default=64)
    parser.add_argument("--vulkan", action="store_true")
    parser.add_argument("--adapter-index", type=int, default=-1)
    return parser.parse_args()


def configure_reference(renderer, caustica, spp: int) -> None:
    renderer.app.set_reference_mode(
        spp=spp,
        oidn=True,
        oidn_quality=int(caustica.OidnQuality.High),
        oidn_passes=int(caustica.OidnPasses.AlbedoNormal),
        oidn_prefilter=int(caustica.OidnPrefilter.Accurate),
    )
    renderer.settings.oidn_use_gpu = True
    renderer.settings.accumulation_prewarm_realtime_caches = False


def configure_realtime(renderer, caustica, denoiser: str) -> str:
    settings = renderer.settings
    settings.enable_animations = True
    settings.enable_keyframes = True
    if denoiser == "dlss-rr" and settings.is_dlss_rr_supported:
        renderer.app.set_realtime_mode(
            standalone_denoiser=False,
            realtime_aa=int(caustica.RealtimeAA.DLSS_RR),
        )
        settings.dlss_mode = int(caustica.DLSSMode.Balanced)
        settings.dlss_rr_preset = int(caustica.DLSSRRPreset.PresetE)
        settings.disable_restirs_with_dlss_rr = True
        label = "DLSS-RR"
    else:
        if denoiser == "dlss-rr":
            print("[caustica] DLSS-RR unavailable; falling back to NRD + TAA.")
        renderer.app.set_realtime_mode(
            standalone_denoiser=True,
            realtime_aa=int(caustica.RealtimeAA.TAA),
        )
        label = "NRD + TAA"
    settings.accumulation_target = 1
    settings.reset_realtime_caches = True
    return label


def main() -> int:
    args = parse_args()
    if args.frames <= 0 or args.fps <= 0 or args.spp <= 0:
        raise ValueError("--frames, --fps and --spp must be positive")

    try:
        import caustica
    except ImportError as exc:
        sys.stderr.write("Install the built wheel before running this example.\n")
        raise exc

    scene = resolve_scene_arg(args.scene)
    out_dir = args.out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    dt = 1.0 / args.fps

    with caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=True,
        vulkan=args.vulkan,
        adapter_index=args.adapter_index,
        scene=scene,
        realtime=(args.mode == "realtime"),
        accumulation_target=args.spp if args.mode == "reference" else 1,
    ) as renderer:
        if args.mode == "reference":
            configure_reference(renderer, caustica, args.spp)
            mode_label = f"Reference {args.spp} SPP + OIDN"
        else:
            mode_label = configure_realtime(renderer, caustica, args.denoiser)
            renderer.app.scene_time = args.start_time

        print(f"[caustica] {mode_label}: {args.frames} frame(s) -> {out_dir}")
        started = time.perf_counter()
        for frame_index in range(args.frames):
            scene_time = args.start_time + frame_index * dt
            if args.mode == "reference":
                if not renderer.prepare_animation_frame(scene_time):
                    raise RuntimeError(f"Failed to prepare animation frame {frame_index}")
                engine_frames = renderer.render_reference_frame(spp=args.spp, oidn=True)
                if not renderer.app.accumulation_completed:
                    raise RuntimeError(
                        f"Reference accumulation did not complete for frame {frame_index} "
                        f"after {engine_frames} engine frames"
                    )
            else:
                engine_frames = 1
                if not renderer.render_realtime_frame(0.0 if frame_index == 0 else dt):
                    raise RuntimeError(f"Realtime render failed for frame {frame_index}")

            output = out_dir / f"frame_{frame_index:06d}.png"
            if not renderer.save_screenshot(str(output)):
                raise RuntimeError(f"Failed to save {output}")
            sample_info = (
                f" sample={renderer.app.accumulation_sample_index}"
                if args.mode == "reference"
                else ""
            )
            print(
                f"[caustica] frame={frame_index:06d} time={scene_time:.6f}s "
                f"engine_frames={engine_frames}{sample_info} saved={output}"
            )

    elapsed = time.perf_counter() - started
    print(f"[caustica] Done in {elapsed:.2f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

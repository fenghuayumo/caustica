#!/usr/bin/env python
"""Render a scene animation at explicit timeline samples.

The two modes differ in how time is advanced, which is the point of the
example. Reference mode freezes each animation time, accumulates the full
sample budget and runs OIDN before moving on, so every output frame is
converged. Realtime mode renders exactly one engine frame per output frame and
keeps NRD or DLSS-RR temporal history alive across frames.

Examples:
    python examples/python/animation_sequence.py --frames 48 --fps 24
    python examples/python/animation_sequence.py --mode realtime --denoiser dlss-rr
"""

from __future__ import annotations

import argparse
import time

from _common import (
    REALTIME_DENOISERS,
    add_device_args,
    add_quality_args,
    apply_realtime_mode,
    apply_reference_mode,
    import_caustica,
    make_engine,
    resolve_output_path,
    resolve_scene_arg,
    save_screenshot,
    validate_positive,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--scene", default="Assets/kitchen.scene.json")
    parser.add_argument("--out-dir", default="sequence")
    parser.add_argument("--mode", choices=["reference", "realtime"], default="reference")
    parser.add_argument(
        "--denoiser",
        choices=REALTIME_DENOISERS,
        default="auto",
        help="Realtime denoiser / AA path; auto selects NRD + TAA.",
    )
    parser.add_argument("--frames", type=int, default=1, help="Number of output frames.")
    parser.add_argument("--fps", type=float, default=24.0, help="Timeline sample rate.")
    parser.add_argument("--start-time", type=float, default=0.0)
    add_device_args(parser)
    add_quality_args(parser)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    validate_positive(args, "width", "height", "spp", "frames", "fps")

    caustica = import_caustica()
    is_reference = args.mode == "reference"
    out_dir = resolve_output_path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    dt = 1.0 / args.fps

    with make_engine(
        caustica,
        args,
        scene=resolve_scene_arg(args.scene),
        realtime=not is_reference,
        headless=True,
        accumulation_target=args.spp if is_reference else 1,
    ) as engine:
        settings = engine.settings
        if is_reference:
            mode_label = apply_reference_mode(engine, caustica, spp=args.spp, oidn=True)
            # Realtime cache prewarming would be discarded at every time step.
            settings.accumulation_prewarm_realtime_caches = False
        else:
            settings.enable_animations = True
            settings.enable_keyframes = True
            mode_label = apply_realtime_mode(engine, caustica, args.denoiser)
            settings.accumulation_target = 1
            settings.reset_realtime_caches = True
            engine.scene_time = args.start_time

        print(f"[caustica] {mode_label}: {args.frames} frame(s) -> {out_dir}")
        started = time.perf_counter()

        for index in range(args.frames):
            scene_time = args.start_time + index * dt
            if is_reference:
                if not engine.prepare_animation_frame(scene_time):
                    raise SystemExit(f"Failed to prepare animation frame {index}")
                engine_frames = engine.render_reference_frame(spp=args.spp, oidn=True)
                if not engine.accumulation_completed:
                    raise SystemExit(
                        f"Reference accumulation did not complete for frame {index} "
                        f"after {engine_frames} engine frames"
                    )
                detail = f" sample={engine.accumulation_sample_index}"
            else:
                # A zero delta on the first frame seeds temporal history.
                if not engine.render_realtime_frame(0.0 if index == 0 else dt):
                    raise SystemExit(f"Realtime render failed for frame {index}")
                engine_frames = 1
                detail = ""

            saved = save_screenshot(engine, out_dir / f"frame_{index:06d}.png")
            print(
                f"[caustica] frame={index:06d} time={scene_time:.6f}s "
                f"engine_frames={engine_frames}{detail} saved={saved.name}"
            )

        print(f"[caustica] Done in {time.perf_counter() - started:.2f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

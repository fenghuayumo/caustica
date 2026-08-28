#!/usr/bin/env python
"""Render one scene through the installed caustica Python extension.

This is the primary extension example. It covers reference, realtime, and
interactive rendering without duplicating a separate script for every mode.

Examples:
    python examples/python/render.py --scene builtin:plane_cube
    python examples/python/render.py --mode realtime --denoiser nrd --frames 32
    python examples/python/render.py --mode window --scene Assets/default.json
    python examples/python/render.py --spawn Assets/Models/GlassSphere/GlassSphere.gltf
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

from _common import (
    frame_bounds,
    resolve_output_path,
    resolve_path,
    resolve_scene_arg,
    run_window_loop,
    scene_bounds_center_radius,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", default="builtin:plane_cube")
    parser.add_argument(
        "--mode",
        choices=["reference", "realtime", "window"],
        default="reference",
        help="Reference accumulation, fixed-count realtime frames, or an interactive window.",
    )
    parser.add_argument(
        "--denoiser",
        choices=["auto", "none", "oidn", "taa", "nrd", "dlss-rr"],
        default="auto",
        help="auto selects OIDN for reference and NRD + TAA for realtime.",
    )
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--spp", type=int, default=64, help="Reference samples per pixel.")
    parser.add_argument("--frames", type=int, default=32, help="Realtime warmup/output frames.")
    parser.add_argument("--bounces", type=int, default=8)
    parser.add_argument("--out", default="frame.png")
    parser.add_argument("--vulkan", action="store_true")
    parser.add_argument(
        "--adapter",
        default="auto",
        help="GPU selector: auto, index:N, name:text, uuid:hex, or luid:hex.",
    )
    parser.add_argument("--spawn", default=None, help="Optional mesh/prefab to spawn after loading.")
    parser.add_argument("--spawn-position", type=float, nargs=3, default=(0.0, 0.0, 0.0))
    parser.add_argument(
        "--material",
        action="append",
        nargs=4,
        metavar=("NAME", "R", "G", "B"),
        help="Override a material base color; may be repeated.",
    )
    parser.add_argument("--camera-pos", type=float, nargs=3)
    parser.add_argument("--camera-dir", type=float, nargs=3)
    parser.add_argument("--camera-up", type=float, nargs=3, default=(0.0, 1.0, 0.0))
    parser.add_argument("--fov", type=float)
    parser.add_argument(
        "--inspect-framebuffer",
        action="store_true",
        help="Read back and print the RGBA8 framebuffer metadata.",
    )
    return parser.parse_args()


def configure_reference(renderer, caustica, args: argparse.Namespace) -> str:
    if args.denoiser not in {"auto", "none", "oidn"}:
        raise ValueError("reference mode supports --denoiser auto, none, or oidn")
    use_oidn = args.denoiser in {"auto", "oidn"}
    renderer.app.set_reference_mode(
        spp=args.spp,
        oidn=use_oidn,
        oidn_quality=int(caustica.OidnQuality.High),
        oidn_passes=int(caustica.OidnPasses.AlbedoNormal),
        oidn_prefilter=int(caustica.OidnPrefilter.Accurate),
    )
    renderer.settings.oidn_use_gpu = True
    if use_oidn:
        renderer.settings.oidn_apply()
    return f"reference {args.spp} spp" + (" + OIDN" if use_oidn else "")


def configure_realtime(renderer, caustica, requested: str) -> str:
    denoiser = "nrd" if requested == "auto" else requested
    if denoiser == "oidn":
        raise ValueError("OIDN is a reference-mode denoiser; use --mode reference")

    settings = renderer.settings
    if denoiser == "dlss-rr":
        if settings.is_dlss_rr_supported:
            renderer.app.set_realtime_mode(
                standalone_denoiser=False,
                realtime_aa=int(caustica.RealtimeAA.DLSS_RR),
            )
            settings.dlss_mode = int(caustica.DLSSMode.Balanced)
            settings.dlss_rr_preset = int(caustica.DLSSRRPreset.PresetE)
            settings.disable_restirs_with_dlss_rr = True
            return "realtime DLSS-RR"
        print("[caustica] DLSS-RR unavailable; falling back to NRD + TAA.")
        denoiser = "nrd"

    if denoiser == "nrd":
        renderer.app.set_realtime_mode(
            standalone_denoiser=True,
            realtime_aa=int(caustica.RealtimeAA.TAA),
        )
        return "realtime NRD + TAA"
    if denoiser == "taa":
        renderer.app.set_realtime_mode(
            standalone_denoiser=False,
            realtime_aa=int(caustica.RealtimeAA.TAA),
        )
        return "realtime TAA"

    renderer.app.set_realtime_mode(
        standalone_denoiser=False,
        realtime_aa=int(caustica.RealtimeAA.Off),
    )
    return "realtime without denoising"


def apply_scene_edits(renderer, args: argparse.Namespace) -> None:
    if args.spawn:
        spawn_path = resolve_path(args.spawn)
        if not spawn_path.exists():
            raise FileNotFoundError(f"spawn asset not found: {spawn_path}")
        entity = renderer.app.spawn_from_file(str(spawn_path))
        if entity is None:
            raise RuntimeError(f"spawn_from_file failed: {spawn_path}")
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

    if args.camera_pos and args.camera_dir:
        renderer.set_camera(
            tuple(args.camera_pos),
            tuple(args.camera_dir),
            tuple(args.camera_up),
        )
    elif args.camera_pos or args.camera_dir:
        raise ValueError("--camera-pos and --camera-dir must be provided together")
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
    if min(args.width, args.height, args.spp, args.frames) <= 0:
        raise ValueError("--width, --height, --spp and --frames must be positive")

    try:
        import caustica
    except ImportError as exc:
        sys.stderr.write("Install the package first: python -m pip install .\n")
        raise exc

    launch_cwd = Path.cwd()
    is_reference = args.mode == "reference"
    is_windowed = args.mode == "window"
    scene = resolve_scene_arg(args.scene)

    with caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=not is_windowed,
        vulkan=args.vulkan,
        adapter=args.adapter,
        scene=scene,
        realtime=not is_reference,
        accumulation_target=args.spp if is_reference else 1,
    ) as renderer:
        settings = renderer.settings
        settings.bounce_count = args.bounces
        settings.use_nee = True
        settings.enable_tone_mapping = True
        mode_label = (
            configure_reference(renderer, caustica, args)
            if is_reference
            else configure_realtime(renderer, caustica, args.denoiser)
        )
        apply_scene_edits(renderer, args)

        print(f"[caustica] Scene: {renderer.app.scene_name}")
        print(f"[caustica] Mode : {mode_label}")
        started = time.perf_counter()
        if is_windowed:
            run_window_loop(renderer)
            return 0
        if is_reference:
            rendered_frames = renderer.step_until_accumulated()
        else:
            if not renderer.step_n(args.frames):
                raise RuntimeError("realtime frame stepping failed")
            rendered_frames = args.frames

        if args.inspect_framebuffer:
            inspect_framebuffer(renderer)
        output = resolve_output_path(args.out, launch_cwd)
        if not renderer.save_screenshot(str(output)):
            raise RuntimeError(f"failed to save screenshot: {output}")
        print(
            f"[caustica] Saved {output} after {rendered_frames} engine frame(s) "
            f"in {time.perf_counter() - started:.2f}s"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

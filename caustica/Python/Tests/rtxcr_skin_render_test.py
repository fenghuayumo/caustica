#!/usr/bin/env python3
"""GPU smoke test for RTXCR Burley diffusion and transmission queries."""

from __future__ import annotations

import argparse
import os
import tempfile
import traceback
from pathlib import Path

from openpbr_white_furnace_render_test import (
    compare_roi,
    configure_common_material,
    make_scene_json,
    render,
    sphere_roi,
    write_constant_hdr,
)


def log(message: str) -> None:
    os.write(1, (message + "\n").encode("utf-8", errors="replace"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--width", type=int, default=64)
    parser.add_argument("--height", type=int, default=64)
    parser.add_argument("--spp", type=int, default=64)
    parser.add_argument("--min-mae", type=float, default=0.25)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    import caustica

    with tempfile.TemporaryDirectory(prefix="caustica-rtxcr-skin-") as temp_dir:
        hdr_path = Path(temp_dir) / "constant_white.hdr"
        write_constant_hdr(hdr_path)
        with caustica.Renderer(
            width=args.width,
            height=args.height,
            headless=True,
            scene=make_scene_json(hdr_path),
            realtime=False,
            accumulation_target=args.spp,
        ) as renderer:
            if not renderer.scene_ready and not renderer.wait_until_ready(
                timeout_seconds=120.0, warmup_frames=4
            ):
                raise RuntimeError("RTXCR skin scene did not become ready")

            renderer.set_camera((0.0, 0.5, 3.0), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0))
            renderer.set_camera_fov(35.0)
            renderer.app.set_reference_mode(spp=args.spp, oidn=False)
            settings = renderer.settings
            settings.bounce_count = 8
            settings.use_nee = True
            settings.use_restir_di = False
            settings.use_restir_gi = False
            settings.enable_tone_mapping = False
            settings.enable_bloom = False
            settings.reference_firefly_filter_enabled = False
            settings.environment_map.enabled = True
            settings.environment_map.visible_to_camera = True
            settings.environment_map.tint_color = (1.0, 1.0, 1.0)
            settings.environment_map.intensity = 0.25

            renderer.step_n(2)
            material = renderer.app.find_material_by_id(0)
            if material is None:
                raise RuntimeError("builtin sphere material ID 0 was unavailable")
            configure_common_material(material)
            material.base_color = (0.72, 0.28, 0.20)
            material.specular_roughness = 0.35
            material.subsurface_color = (1.0, 0.48, 0.32)
            material.subsurface_radius = 0.5
            material.subsurface_radius_scale = (1.0, 0.5, 0.25)
            material.subsurface_scatter_anisotropy = 0.2

            material.subsurface_weight = 0.0
            material.mark_dirty()
            baseline = render(renderer, args.output_dir / "skin_off.png", args.spp)

            material.subsurface_weight = 1.0
            material.mark_dirty()
            skin = render(renderer, args.output_dir / "skin_on.png", args.spp)

            indices = sphere_roi(args.width, args.height)
            _, mean_absolute, _ = compare_roi(baseline, skin, indices)
            channel_means = tuple(
                sum(skin[pixel * 4 + channel] for pixel in indices) / len(indices)
                for channel in range(3)
            )
            mean_skin = sum(channel_means) / 3.0
            log(
                f"RTXCR skin: roi_pixels={len(indices)} "
                f"rgb=({channel_means[0]:.4f}, {channel_means[1]:.4f}, "
                f"{channel_means[2]:.4f}) mean={mean_skin:.4f} "
                f"baseline_mae={mean_absolute:.4f}"
            )
            if mean_skin <= 1.0:
                raise RuntimeError("RTXCR skin render is black")
            if mean_absolute < args.min_mae:
                raise RuntimeError(
                    f"subsurface branch had no measurable effect: "
                    f"MAE {mean_absolute:.4f} < {args.min_mae:.4f}"
                )
            if not (
                channel_means[0] > channel_means[1] + 5.0
                and channel_means[1] > channel_means[2] + 2.0
            ):
                raise RuntimeError(
                    "RTXCR RGB scattering radii did not produce the expected "
                    f"skin chroma: {channel_means!r}"
                )

    log(f"RTXCR skin render smoke test passed; artifacts: {args.output_dir.resolve()}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        os.write(2, traceback.format_exc().encode("utf-8", errors="replace"))
        raise SystemExit(2)

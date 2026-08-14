#!/usr/bin/env python3
"""Render smoke test for the synthetic combined RTXCR skin + DOTS hair scene."""

from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import traceback
from pathlib import Path

from openpbr_white_furnace_render_test import compare_roi, render


def log(message: str) -> None:
    os.write(1, (message + "\n").encode("utf-8", errors="replace"))


def full_roi(width: int, height: int) -> list[int]:
    return [y * width + x for y in range(height) for x in range(width)]


def configure_renderer(renderer: object, spp: int) -> None:
    renderer.set_camera((0.0, 1.30, 5.2), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0))
    renderer.set_camera_fov(27.5)
    renderer.app.set_reference_mode(spp=spp, oidn=False)
    settings = renderer.settings
    settings.bounce_count = 8
    settings.use_nee = True
    settings.use_restir_di = False
    settings.use_restir_gi = False
    settings.enable_tone_mapping = True
    settings.enable_bloom = False


def cached_materials(renderer: object) -> dict[str, object]:
    materials = [renderer.app.find_material_by_id(index) for index in range(32)]
    return {material.name: material for material in materials if material is not None}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--width", type=int, default=160)
    parser.add_argument("--height", type=int, default=160)
    parser.add_argument("--spp", type=int, default=64)
    parser.add_argument("--min-skin-mae", type=float, default=0.12)
    parser.add_argument("--min-hair-mae", type=float, default=0.18)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    import caustica

    root = Path(__file__).resolve().parents[3]
    sys.path.insert(0, str(root / "tools"))
    from generate_rtxcr_character_validation_asset import generate

    temp_assets = tempfile.TemporaryDirectory(prefix="caustica-rtxcr-character-")
    asset_root = Path(temp_assets.name)
    _, skin_model, scene = generate(asset_root)
    skin_only_scene = json.loads(scene.read_text(encoding="utf-8"))
    skin_only_scene["models"] = [str(skin_model)]
    with caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=True,
        scene=json.dumps(skin_only_scene),
        realtime=False,
        accumulation_target=args.spp,
    ) as renderer:
        if not renderer.scene_ready and not renderer.wait_until_ready(timeout_seconds=120.0, warmup_frames=4):
            raise RuntimeError("RTXCR skin-only validation scene did not become ready")
        configure_renderer(renderer, args.spp)
        skin = cached_materials(renderer).get("ValidationSkin")
        if skin is None:
            raise RuntimeError("skin-only validation material unavailable")
        skin.subsurface_weight = 1.0
        skin.mark_dirty()
        skin_only = render(renderer, args.output_dir / "character_skin_only.png", args.spp)

    with caustica.Renderer(
        width=args.width,
        height=args.height,
        headless=True,
        scene=str(scene),
        realtime=False,
        accumulation_target=args.spp,
    ) as renderer:
        if not renderer.scene_ready and not renderer.wait_until_ready(timeout_seconds=120.0, warmup_frames=4):
            raise RuntimeError("RTXCR character validation scene did not become ready")

        configure_renderer(renderer, args.spp)

        material_by_name = cached_materials(renderer)
        skin = material_by_name.get("ValidationSkin")
        hair = material_by_name.get("ValidationHair")
        if skin is None or hair is None:
            raise RuntimeError(
                "validation materials unavailable: "
                f"skin={skin!r}, hair={hair!r}, names={sorted(material_by_name)}"
            )
        if skin.subsurface_weight < 0.99:
            raise RuntimeError("NV_materials_subsurface was not mapped to OpenPBR subsurface_weight")
        if abs(skin.subsurface_radius - 0.55) > 1e-4:
            raise RuntimeError(f"unexpected imported subsurface radius: {skin.subsurface_radius}")

        skin.subsurface_weight = 0.0
        skin.mark_dirty()
        skin_off = render(renderer, args.output_dir / "character_skin_off.png", args.spp)

        skin.subsurface_weight = 1.0
        skin.mark_dirty()
        combined = render(renderer, args.output_dir / "character_skin_hair.png", args.spp)

        roi = full_roi(args.width, args.height)
        _, skin_mae, _ = compare_roi(skin_off, combined, roi)
        _, hair_mae, _ = compare_roi(skin_only, combined, roi)
        mean_combined = sum(combined[offset] + combined[offset + 1] + combined[offset + 2]
                            for offset in range(0, len(combined), 4)) / (args.width * args.height * 3)
        log(f"RTXCR character: skin_mae={skin_mae:.4f} hair_mae={hair_mae:.4f} mean={mean_combined:.4f}")
        if skin_mae < args.min_skin_mae:
            raise RuntimeError(f"skin branch had no measurable effect: {skin_mae:.4f}")
        if hair_mae < args.min_hair_mae:
            raise RuntimeError(f"hair geometry/shading had no measurable effect: {hair_mae:.4f}")
        if mean_combined < 2.0:
            raise RuntimeError("combined character render is black")

    log(f"RTXCR character render smoke test passed; artifacts: {args.output_dir.resolve()}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        os.write(2, traceback.format_exc().encode("utf-8", errors="replace"))
        raise SystemExit(2)

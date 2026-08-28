#!/usr/bin/env python3
"""GPU image regression for OpenPBR coat/fuzz white-furnace layering.

The unlayered white sphere is rendered first and used as a paired golden.  The
same renderer, camera, constant white HDR environment, sample count, and base
material are then reused for coat/fuzz variants.  Comparing only a conservative
interior disk of the sphere makes the test independent of background pixels and
silhouette antialiasing while still covering a useful range of view angles.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import struct
import tempfile
import traceback
import zlib
from pathlib import Path


def write_rgba8_png(path: Path, width: int, height: int, pixels: bytes) -> None:
    expected = width * height * 4
    if len(pixels) != expected:
        raise ValueError(f"expected {expected} RGBA8 bytes, got {len(pixels)}")

    def chunk(kind: bytes, data: bytes) -> bytes:
        payload = kind + data
        return struct.pack(">I", len(data)) + payload + struct.pack(">I", zlib.crc32(payload))

    rows = b"".join(
        b"\0" + pixels[y * width * 4 : (y + 1) * width * 4]
        for y in range(height)
    )
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows, level=6))
    png += chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def write_constant_hdr(path: Path, width: int = 4, height: int = 2) -> None:
    """Write a small non-RLE Radiance RGBE image containing linear white."""
    header = f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {height} +X {width}\n".encode("ascii")
    # RGBE (128, 128, 128, 129) decodes to exactly linear (1, 1, 1).
    path.write_bytes(header + bytes((128, 128, 128, 129)) * width * height)


def make_scene_json(environment_path: Path) -> str:
    scene = {
        "settings": {"realtimeMode": False},
        "entities": [
            {
                "id": "FurnaceSphere",
                "name": "FurnaceSphere",
                "components": {
                    "PrefabInstance": {"source": "builtin:sphere"},
                },
            },
            {
                "id": "WhiteFurnace",
                "name": "WhiteFurnace",
                "components": {
                    "EnvironmentLight": {
                        "radianceScale": [1.0, 1.0, 1.0],
                        "source": str(environment_path.resolve()),
                    },
                },
            },
            {
                "id": "FurnaceCamera",
                "name": "FurnaceCamera",
                "components": {
                    "Transform": {
                        "translation": [0.0, 0.5, 3.0],
                        "rotation": [0.0, 0.0, 0.0, 1.0],
                    },
                    "PerspectiveCameraEx": {
                        "verticalFov": math.radians(35.0),
                        "zNear": 0.001,
                        "exposureCompensation": 0.0,
                        "enableAutoExposure": False,
                    },
                },
            },
        ],
    }
    return json.dumps(scene)


def configure_common_material(material: object) -> None:
    material.material_model = "OpenPBR"
    material.base_color = (1.0, 1.0, 1.0)
    material.base_weight = 1.0
    material.base_metalness = 0.0
    material.base_diffuse_roughness = 0.0
    material.specular_color = (1.0, 1.0, 1.0)
    material.specular_weight = 1.0
    material.specular_roughness = 0.0
    material.specular_ior = 1.5
    material.transmission_weight = 0.0
    material.transmission_diffuse_weight = 0.0
    material.subsurface_weight = 0.0
    material.thin_film_weight = 0.0
    material.emission_color = (0.0, 0.0, 0.0)
    material.emission_luminance = 0.0
    material.geometry_opacity = 1.0
    material.enable_base_color_texture = False
    material.enable_base_metalness_specular_roughness_texture = False
    material.enable_geometry_normal_texture = False
    material.enable_emission_color_texture = False
    material.enable_transmission_weight_texture = False


def verify_legacy_material_migration(material: object) -> None:
    """Guard the RTXPT-compatible defaults before authoring the furnace case."""
    specular_color = tuple(float(channel) for channel in material.specular_color)
    if any(abs(channel) > 1.0e-6 for channel in specular_color):
        raise RuntimeError(
            "legacy builtin material gained a dielectric specular tint: "
            f"specular_color={specular_color}"
        )

    specular_roughness = float(material.specular_roughness)
    diffuse_roughness = float(material.base_diffuse_roughness)
    if abs(diffuse_roughness - specular_roughness) > 1.0e-6:
        raise RuntimeError(
            "legacy builtin material did not migrate roughness to "
            "base_diffuse_roughness: "
            f"specular={specular_roughness}, diffuse={diffuse_roughness}"
        )


CASES = (
    ("coat_smooth", {"coat_weight": 1.0, "coat_roughness": 0.08}),
    ("coat_rough", {"coat_weight": 1.0, "coat_roughness": 0.45}),
    ("fuzz", {"fuzz_weight": 1.0, "fuzz_roughness": 0.55}),
    (
        "coat_fuzz",
        {
            "coat_weight": 1.0,
            "coat_roughness": 0.25,
            "fuzz_weight": 1.0,
            "fuzz_roughness": 0.55,
        },
    ),
)


def apply_case(material: object, values: dict[str, float]) -> None:
    material.coat_weight = 0.0
    material.coat_color = (1.0, 1.0, 1.0)
    material.coat_roughness = 0.0
    material.coat_ior = 1.5
    material.coat_darkening = 0.0
    material.fuzz_weight = 0.0
    material.fuzz_color = (1.0, 1.0, 1.0)
    material.fuzz_roughness = 0.5
    for name, value in values.items():
        setattr(material, name, value)
    material.mark_dirty()


def sphere_roi(width: int, height: int) -> list[int]:
    cx = (width - 1) * 0.5
    cy = (height - 1) * 0.5
    # The projected radius is about 0.26 * height. Stay inside 72% of it to
    # avoid silhouette pixels while sampling normals out to roughly 45 degrees.
    radius = min(width, height) * 0.185
    radius2 = radius * radius
    return [
        y * width + x
        for y in range(height)
        for x in range(width)
        if (x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius2
    ]


def compare_roi(golden: bytes, actual: bytes, indices: list[int]) -> tuple[float, float, float]:
    squared = 0.0
    absolute = 0.0
    signed = 0.0
    count = len(indices) * 3
    for pixel in indices:
        offset = pixel * 4
        for channel in range(3):
            delta = float(actual[offset + channel]) - float(golden[offset + channel])
            squared += delta * delta
            absolute += abs(delta)
            signed += delta
    return math.sqrt(squared / count), absolute / count, signed / count


def difference_image(golden: bytes, actual: bytes, scale: float = 4.0) -> bytes:
    result = bytearray(len(golden))
    for offset in range(0, len(golden), 4):
        for channel in range(3):
            result[offset + channel] = min(255, round(abs(actual[offset + channel] - golden[offset + channel]) * scale))
        result[offset + 3] = 255
    return bytes(result)


def render(renderer: object, output: Path, spp: int) -> bytes:
    renderer.app.reset_accumulation()
    frames = renderer.step_until_accumulated()
    if frames <= 0 or not renderer.app.accumulation_completed:
        raise RuntimeError(f"reference accumulation did not complete (frames={frames}, spp={spp})")
    framebuffer = renderer.get_framebuffer()
    pixels = bytes(framebuffer.pixels)
    write_rgba8_png(output, framebuffer.width, framebuffer.height, pixels)
    return pixels


def log(message: str) -> None:
    # The embedded app may replace Python's sys.stdout. Writing the process
    # descriptor keeps CTest diagnostics visible.
    os.write(1, (message + "\n").encode("utf-8", errors="replace"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--width", type=int, default=96)
    parser.add_argument("--height", type=int, default=96)
    parser.add_argument("--spp", type=int, default=128)
    parser.add_argument("--max-rmse", type=float, default=17.0)
    parser.add_argument("--max-mean-absolute-error", type=float, default=12.0)
    # This is the primary white-furnace energy threshold. The pre-fix fuzz
    # implementation measured about +12.6/255 here; the corrected path is <2.
    parser.add_argument("--max-mean-bias", type=float, default=4.0)
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    import caustica

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="caustica-white-furnace-") as temp_dir:
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
            if not renderer.scene_ready and not renderer.wait_until_ready(timeout_seconds=120.0, warmup_frames=4):
                raise RuntimeError("white-furnace scene did not become ready")

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

            # Builtin materials live in the runtime material cache rather than
            # Scene.materials, so use the cache-backed ID lookup.
            renderer.step_n(2)
            material = renderer.app.find_material_by_id(0)
            if material is None:
                raise RuntimeError("Mat_BuiltinSphere was not available as material ID 0")
            verify_legacy_material_migration(material)
            configure_common_material(material)

            apply_case(material, {})
            golden = render(renderer, args.output_dir / "golden_unlayered.png", args.spp)
            indices = sphere_roi(args.width, args.height)
            if not indices:
                raise RuntimeError("empty sphere ROI")

            for name, values in CASES:
                apply_case(material, values)
                actual = render(renderer, args.output_dir / f"{name}.png", args.spp)
                rmse, mean_absolute, mean_bias = compare_roi(golden, actual, indices)
                write_rgba8_png(
                    args.output_dir / f"{name}_diff_x4.png",
                    args.width,
                    args.height,
                    difference_image(golden, actual),
                )
                log(
                    f"{name}: roi_pixels={len(indices)} rmse={rmse:.4f} "
                    f"mae={mean_absolute:.4f} bias={mean_bias:+.4f}"
                )
                if rmse > args.max_rmse:
                    failures.append(f"{name} RMSE {rmse:.4f} > {args.max_rmse:.4f}")
                if mean_absolute > args.max_mean_absolute_error:
                    failures.append(
                        f"{name} mean absolute error {mean_absolute:.4f} > "
                        f"{args.max_mean_absolute_error:.4f}"
                    )
                if abs(mean_bias) > args.max_mean_bias:
                    failures.append(
                        f"{name} mean bias {mean_bias:+.4f} exceeds +/-{args.max_mean_bias:.4f}"
                    )

    if failures:
        log("OpenPBR white-furnace render regression FAILED:")
        for failure in failures:
            log(f"  - {failure}")
        log(f"Artifacts: {args.output_dir.resolve()}")
        return 1

    log(f"OpenPBR white-furnace render regression passed; artifacts: {args.output_dir.resolve()}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        os.write(2, traceback.format_exc().encode("utf-8", errors="replace"))
        raise SystemExit(2)

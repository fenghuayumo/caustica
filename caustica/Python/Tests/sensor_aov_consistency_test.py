#!/usr/bin/env python3
"""P0-2: Sensor RGB matches get_pixels; linear_rgb is opt-in HDR."""

from __future__ import annotations

import argparse
import os
import traceback
from pathlib import Path

from openpbr_white_furnace_render_test import write_rgba8_png
from sensor_warmup_test import as_uint8_rgb, mean_rgb


def log(message: str) -> None:
    os.write(1, (message + "\n").encode("utf-8", errors="replace"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--height", type=int, default=192)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    import numpy as np
    import caustica

    failures: list[str] = []
    with caustica.EngineApp.create(
        width=args.width,
        height=args.height,
        headless=True,
        scene="builtin:plane_cube",
        realtime=True,
    ) as engine:
        if not engine.is_scene_ready and not engine.wait_until_ready(timeout_seconds=120.0):
            raise RuntimeError("plane_cube scene did not become ready")

        engine.add_render_product(
            "ldr", None, int(caustica.Aov.rgb) | int(caustica.Aov.depth)
        )
        ldr_products = engine.capture_sensor_outputs()
        if not ldr_products:
            raise RuntimeError("capture_sensor_outputs returned no products")
        product = ldr_products[0]
        pixels = as_uint8_rgb(engine.get_pixels())
        rgb = as_uint8_rgb(product.rgb)
        write_rgba8_png(
            args.output_dir / "sensor_rgb.png",
            int(rgb.shape[1]),
            int(rgb.shape[0]),
            rgb.tobytes(),
        )
        if rgb.shape != pixels.shape:
            failures.append(f"rgb shape {rgb.shape} != get_pixels {pixels.shape}")
        elif not np.array_equal(rgb[..., :3], pixels[..., :3]):
            mae = float(
                np.abs(rgb[..., :3].astype(np.float32) - pixels[..., :3].astype(np.float32)).mean()
            )
            failures.append(f"product.rgb is not get_pixels: MAE {mae:.3f}")
        if product.linear_rgb is not None:
            failures.append("linear_rgb should be None when Aov.linear_rgb was not requested")

        engine.add_render_product(
            "hdr",
            None,
            int(caustica.Aov.rgb) | int(caustica.Aov.depth) | int(caustica.Aov.linear_rgb),
        )
        hdr_products = engine.capture_sensor_outputs()
        hdr_product = next((item for item in hdr_products if item.name == "hdr"), None)
        if hdr_product is None:
            raise RuntimeError("hdr render product missing")
        linear = hdr_product.linear_rgb
        depth = hdr_product.depth
        if linear is None:
            failures.append("linear_rgb is None after requesting Aov.linear_rgb")
        else:
            linear = np.asarray(linear, dtype=np.float32)
            log(
                f"linear_rgb mean={float(linear.mean()):.5f} max={float(linear.max()):.5f}"
            )
            if float(linear.max()) <= 0.0:
                failures.append("linear_rgb on a lit cube+ground should be > 0")
            if depth is not None:
                depth = np.asarray(depth, dtype=np.float32)
                if depth.shape[:2] == linear.shape[:2]:
                    miss = depth <= 0.0
                    if np.any(miss) and float(np.max(linear[miss])) > 1e-6:
                        failures.append("linear_rgb miss pixels are not 0")

        log(f"get_pixels mean RGB = {mean_rgb(pixels):.3f}")

    if failures:
        for item in failures:
            log(f"FAIL: {item}")
        return 1
    log("sensor_aov_consistency_test: ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        traceback.print_exc()
        raise SystemExit(1)

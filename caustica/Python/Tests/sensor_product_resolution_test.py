#!/usr/bin/env python3
"""P0-3: wrist and third-person RenderProducts at different resolutions in one capture."""

from __future__ import annotations

import argparse
import json
import math
import os
import traceback
from pathlib import Path

from openpbr_white_furnace_render_test import write_rgba8_png
from sensor_warmup_test import as_uint8_rgb, mean_rgb


def log(message: str) -> None:
    os.write(1, (message + "\n").encode("utf-8", errors="replace"))


def make_two_camera_scene() -> str:
    camera = {
        "verticalFov": math.radians(35.0),
        "zNear": 0.001,
        "exposureCompensation": 0.0,
        "enableAutoExposure": False,
    }
    return json.dumps(
        {
            "settings": {"realtimeMode": True},
            "entities": [
                {
                    "id": "Ground",
                    "name": "Ground",
                    "components": {"PrefabInstance": {"source": "builtin:plane"}},
                },
                {
                    "id": "Cube",
                    "name": "Cube",
                    "components": {
                        "PrefabInstance": {"source": "builtin:cube"},
                        "Transform": {"translation": [0.0, 0.5, 0.0]},
                    },
                },
                {
                    "id": "Sun",
                    "name": "Sun",
                    "components": {
                        "Transform": {
                            "rotation": [-0.23053891, -0.15879166, -0.6890466, 0.6684697],
                        },
                        "DirectionalLight": {
                            "angularSize": 1.5,
                            "color": [1.0, 0.96, 0.9],
                            "irradiance": 4.0,
                        },
                    },
                },
                {
                    "id": "wrist",
                    "name": "wrist",
                    "components": {
                        "Transform": {"translation": [0.35, 0.85, 1.4]},
                        "PerspectiveCameraEx": camera,
                    },
                },
                {
                    "id": "third",
                    "name": "third",
                    "components": {
                        "Transform": {"translation": [0.0, 1.2, 4.0]},
                        "PerspectiveCameraEx": camera,
                    },
                },
            ],
        }
    )


def aov_shape(array, fallback_hw: tuple[int, int]) -> tuple[int, int]:
    import numpy as np

    if array is None:
        return fallback_hw
    arr = np.asarray(array)
    return int(arr.shape[0]), int(arr.shape[1])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    import caustica

    failures: list[str] = []
    with caustica.EngineApp.create(
        width=1280,
        height=720,
        headless=True,
        scene=make_two_camera_scene(),
        realtime=True,
    ) as engine:
        if not engine.is_scene_ready and not engine.wait_until_ready(timeout_seconds=120.0):
            raise RuntimeError("two-camera scene did not become ready")

        wrist = engine.find_entity("wrist")
        third = engine.find_entity("third")
        if wrist is None or third is None:
            raise RuntimeError("wrist/third cameras missing")
        third.activate()
        engine.set_camera_pos_dir_up((0.0, 1.2, 4.0), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0))
        wrist.look_to((0.35, 0.85, 1.4), (0.0, -0.2, -1.0), (0.0, 1.0, 0.0))

        aovs = int(caustica.Aov.rgb) | int(caustica.Aov.depth)
        engine.add_render_product("wrist", wrist, aovs, width=640, height=480)
        engine.add_render_product("third", third, aovs, width=1280, height=720)

        products = {item.name: item for item in engine.capture_sensor_outputs()}
        if "wrist" not in products or "third" not in products:
            raise RuntimeError(f"missing products: {sorted(products)}")

        expected = {"wrist": (640, 480), "third": (1280, 720)}
        for name, (width, height) in expected.items():
            product = products[name]
            rgb = as_uint8_rgb(product.rgb)
            write_rgba8_png(
                args.output_dir / f"{name}.png",
                int(rgb.shape[1]),
                int(rgb.shape[0]),
                rgb.tobytes(),
            )
            log(
                f"{name}: rgb={product.width}x{product.height} "
                f"array={rgb.shape} mean={mean_rgb(rgb):.3f} "
                f"geom={product.geometry_width}x{product.geometry_height}"
            )
            if product.width != width or product.height != height:
                failures.append(
                    f"{name} RGB size {product.width}x{product.height}, expected {width}x{height}"
                )
            if rgb.shape[0] != height or rgb.shape[1] != width:
                failures.append(f"{name} rgb array shape {rgb.shape}, expected ({height}, {width}, 4)")
            if mean_rgb(rgb) < 4.0:
                failures.append(f"{name} RGB too dark: mean {mean_rgb(rgb):.3f}")

            depth_h, depth_w = aov_shape(
                product.depth,
                (
                    product.geometry_height or product.height,
                    product.geometry_width or product.width,
                ),
            )
            if product.depth is None:
                failures.append(f"{name} depth is None")
            elif (depth_w, depth_h) != (width, height) and (
                depth_w != (product.geometry_width or width)
                or depth_h != (product.geometry_height or height)
            ):
                failures.append(
                    f"{name} depth shape {(depth_h, depth_w)} does not match RGB "
                    f"{height}x{width} or geometry "
                    f"{product.geometry_height}x{product.geometry_width}"
                )
            else:
                log(f"{name}: depth shape {(depth_h, depth_w)}")

        if products["wrist"].width == products["third"].width:
            failures.append("wrist and third RGB widths are the same; per-product size did not apply")

    if failures:
        for item in failures:
            log(f"FAIL: {item}")
        return 1
    log("sensor_product_resolution_test: ok")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception:
        traceback.print_exc()
        raise SystemExit(1)

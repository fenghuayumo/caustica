#!/usr/bin/env python3
"""Exercise GPU/plugin teardown and reinitialization within one Python process."""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vulkan", action="store_true")
    parser.add_argument("--debug", action="store_true")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--cycles", type=int, default=8)
    args = parser.parse_args()
    if args.cycles < 2:
        parser.error("cycles must be at least two")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    import caustica
    import numpy as np
    from PIL import Image

    retained = []
    snapshots = []
    for cycle in range(args.cycles):
        width, height = 321 + cycle % 2 * 12, 257
        print(f"cycle={cycle} create", flush=True)
        with caustica.EngineApp.create(width=width, height=height, headless=True,
                                       vulkan=args.vulkan, debug=args.debug,
                                       scene="builtin:plane_cube", realtime=True) as engine:
            if not engine.wait_until_ready(timeout_seconds=120.0):
                raise RuntimeError("scene did not become ready")
            engine.add_render_product("main", None,
                                      int(caustica.Aov.rgb) | int(caustica.Aov.depth))
            for capture in range(2):
                print(f"cycle={cycle} capture={capture}", flush=True)
                if not engine.step_frame():
                    raise RuntimeError("step_frame failed")
                output, = engine.capture_sensor_outputs()
                rgb, depth = np.asarray(output.rgb), np.asarray(output.depth)
                if rgb.shape != (height, width, 4) or depth.shape[:2] != (height, width):
                    raise RuntimeError("invalid RGB/depth shape")
                if not rgb[..., :3].any() or not np.isfinite(depth).all() or not (depth > 0).any():
                    raise RuntimeError("empty or invalid rendered output")
                snapshots.extend(((rgb, rgb.copy()), (depth, depth.copy())))
                retained.extend((engine.scene, output))
            pixels = np.asarray(engine.get_pixels())
            path = args.output_dir / "screenshots_截图" / f"cycle_{cycle}_视图.png"
            if not engine.save_screenshot(str(path)):
                raise RuntimeError("save_screenshot failed")
            with Image.open(path) as png:
                if not np.array_equal(np.asarray(png.convert("RGBA")), pixels):
                    raise RuntimeError("screenshot differs from framebuffer")
            print(f"cycle={cycle} shutdown (old wrappers/arrays retained)", flush=True)
        for actual, expected in snapshots:
            if not np.array_equal(actual, expected):
                raise RuntimeError("previous session's retained readback changed")
        print(f"cycle={cycle} shutdown complete", flush=True)
    retained.clear()
    print("engine recreation passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

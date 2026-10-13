#!/usr/bin/env python3
"""Verify terminal-device-loss handling with a scene known to trigger a GPU fault.

This negative test verifies recovery behavior, not rendering stability. Run it in
a timeout-bounded child process. Exit 77 means the fault was not reproduced.
"""

from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--vulkan", action="store_true")
    parser.add_argument("--iterations", type=int, default=10)
    args = parser.parse_args()
    if args.iterations < 1:
        parser.error("iterations must be positive")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    import caustica

    triggered = False
    retained = []
    with caustica.EngineApp.create(width=640, height=480, headless=True,
                                  vulkan=args.vulkan, scene=args.scene, realtime=True) as engine:
        assert engine.wait_until_ready(timeout_seconds=120.0)
        retained.append(engine.scene)
        for index in range(3):
            retained.append(engine.spawn_camera(
                name=f"loss_probe_{index}", local_translation=(0.2 + index * 0.1, 1.0, 2.0),
                aovs=int(caustica.Aov.rgb) | int(caustica.Aov.depth)))
        for index in range(args.iterations):
            print(f"iteration={index} capture", flush=True)
            if not engine.step_frame():
                break
            try:
                engine.capture_sensor_outputs()
            except RuntimeError as error:
                print(f"capture failed: {error}", flush=True)
                triggered = True
                break
        if triggered:
            assert not engine.step_frame(), "lost session accepted another frame"
            assert not engine.save_screenshot(str(args.output_dir / "lost.png")), \
                "lost session reported a successful screenshot"
            try:
                engine.get_pixels()
            except RuntimeError as error:
                assert "GPU device lost" in str(error), str(error)
            else:
                raise AssertionError("lost session returned pixels")
            try:
                engine.capture_sensor_outputs()
            except RuntimeError as error:
                assert "GPU device lost or removed" in str(error), str(error)
            else:
                raise AssertionError("lost session accepted another capture")
        print("shutdown begin with retained scene/camera wrappers", flush=True)
    retained.clear()
    print("shutdown end", flush=True)
    if not triggered:
        print("GPU fault not reproduced; loss recovery check is inconclusive", flush=True)
        return 77
    print("Loss recovery verified; the underlying GPU fault remains", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

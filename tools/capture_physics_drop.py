#!/usr/bin/env python
"""Capture timed screenshots of physics-drop.scene.json."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bin"))
sys.path.insert(0, str(ROOT / "examples" / "python"))

from _common import apply_realtime_mode, import_caustica, save_screenshot  # noqa: E402

OUT_DIR = ROOT / "docs"
DT = 1.0 / 60.0


def main() -> int:
    caustica = import_caustica()
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    scene = str((ROOT / "assets-builtin" / "scenes" / "physics-drop" / "physics-drop.scene.json").resolve())

    engine = caustica.EngineApp.create(
        width=1280,
        height=720,
        headless=True,
        scene=scene,
        realtime=True,
        accumulation_target=1,
    )
    apply_realtime_mode(engine, caustica, "taa")
    engine.set_camera_pos_dir_up((0.0, 6.0, 11.0), (0.0, -0.45, -0.89), (0.0, 1.0, 0.0))

    names = ("FallingCube", "FallingCubeB", "BouncingSphere", "Ground")

    def report(label: str) -> None:
        bits = []
        for name in names:
            entity = engine.find_entity(name)
            if entity is None:
                bits.append(f"{name}=missing")
                continue
            y = entity.translation[1]
            bits.append(f"{name}.y={y:.3f}")
        print(f"[physics-drop] {label}: " + ", ".join(bits))

    shots = [
        ("physics-drop-start.png", 0, "start"),
        ("physics-drop-falling.png", 24, "falling"),
        ("physics-drop-rest.png", 120, "rest"),
    ]
    for name, extra_frames, label in shots:
        if extra_frames and not engine.step_n(extra_frames):
            raise SystemExit(f"step_n({extra_frames}) failed before {label}")
        if not engine.step_frame(DT):
            raise SystemExit(f"step_frame failed before {label}")
        report(label)
        out = save_screenshot(engine, OUT_DIR / name, launch_cwd=ROOT)
        print(f"[physics-drop] saved {label}: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

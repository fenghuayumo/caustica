from __future__ import annotations

"""
UE-style RT PSO precache for the closed PT feature-preset matrix.

Offline cook (cook_shaders.py) produces DXC shader *libraries*.
This step runs a headless GPU session and CreateStateObject for every
preset so CI/cook machines validate that cooked bins link into RT PSOs.

Runtime does *not* idle-warm these on the interactive frame loop —
it CreateStateObjects only the active preset on first use (or calls this
API explicitly at load time).
"""

import argparse
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN_DIR = ROOT / "bin"


def configure_import_path() -> None:
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(BIN_DIR))
    sys.path.insert(0, str(BIN_DIR))


def precache(*, shader_api: str, scene: str, frames_before: int) -> int:
    configure_import_path()
    import caustica  # type: ignore

    renderer = caustica.Renderer(
        width=64,
        height=64,
        headless=True,
        vulkan=shader_api == "vulkan",
        scene=scene,
        realtime=False,
        accumulation_target=1,
    )
    try:
        # Establish scene extract + hit-group set before CreateStateObject.
        renderer.step_n(max(1, frames_before))
        ready = int(renderer.precache_rt_feature_presets(show_progress=True))
        print(f"[caustica] RT preset precache ready={ready}")
        return 0 if ready > 0 else 1
    finally:
        renderer.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Precache CreateStateObject for every cooked PT feature preset (GPU)."
    )
    parser.add_argument(
        "--shader-api",
        choices=["d3d12", "vulkan"],
        default="d3d12" if os.name == "nt" else "vulkan",
    )
    parser.add_argument("--scene", default="builtin:plane_cube")
    parser.add_argument(
        "--frames-before",
        type=int,
        default=1,
        help="Frames to render before precache so hit groups exist.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    return precache(
        shader_api=args.shader_api,
        scene=args.scene,
        frames_before=args.frames_before,
    )


if __name__ == "__main__":
    raise SystemExit(main())

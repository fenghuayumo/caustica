from __future__ import annotations

import argparse
import os
from pathlib import Path

from build_wheel import BIN_DIR, write_shader_pack


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build caustica shader pack files from the current bin/ shader outputs."
    )
    parser.add_argument(
        "--shader-api",
        choices=["d3d12", "vulkan", "both"],
        default="d3d12" if os.name == "nt" else "vulkan",
    )
    parser.add_argument(
        "--dynamic-shaders",
        choices=["bin", "none"],
        default="bin",
        help="Include ShaderDynamic/Bin entries in the pack when available.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=BIN_DIR,
        help="Directory where caustica.shaders.<api>.pack is written.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    shader_types = ["dxil"] if args.shader_api == "d3d12" else ["spirv"] if args.shader_api == "vulkan" else ["dxil", "spirv"]
    for shader_type in shader_types:
        write_shader_pack(shader_type, args.dynamic_shaders, args.output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


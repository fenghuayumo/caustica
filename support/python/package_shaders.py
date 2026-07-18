from __future__ import annotations

import argparse
import os
from pathlib import Path

from build_wheel import BIN_DIR, write_shader_pack


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build caustica shader pack files. "
            "By default this cooks the coverage PT matrix first (see cook_shaders.py)."
        )
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
    parser.add_argument(
        "--precompile-pt",
        dest="precompile_pt",
        action="store_true",
        default=True,
        help="Run offline path-tracing coverage cook before building packs (default).",
    )
    parser.add_argument(
        "--no-precompile-pt",
        dest="precompile_pt",
        action="store_false",
        help="Skip PT precompile (pack existing bins only).",
    )
    parser.add_argument(
        "--global-preset",
        choices=["default", "coverage"],
        default="coverage",
        help="Global macro preset when PT precompile is enabled.",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Force recompilation of path-tracing shader bins when precompile is enabled.",
    )
    parser.add_argument(
        "--skip-verify",
        action="store_true",
        help="Skip coverage verification after precompile.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.precompile_pt:
        from precompile_pt_shader_bins import run_pt_shader_precompile

        run_pt_shader_precompile(
            args.shader_api,
            force=args.force,
            global_preset=args.global_preset,
        )
        if not args.skip_verify:
            from verify_pt_shader_bins import verify_apis

            rc = verify_apis(args.shader_api, args.global_preset)
            if rc != 0:
                return rc

    shader_types = (
        ["dxil"]
        if args.shader_api == "d3d12"
        else ["spirv"]
        if args.shader_api == "vulkan"
        else ["dxil", "spirv"]
    )
    for shader_type in shader_types:
        write_shader_pack(shader_type, args.dynamic_shaders, args.output_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

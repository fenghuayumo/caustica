from __future__ import annotations

"""
Official Caustica PT shader cook entry point (UE-style offline specialization).

Pipeline:
  1) DXC-precompile closed feature-preset matrix into ShaderDynamic/Bin
  2) Verify every cooked bin exists
  3) Package caustica.shaders.<api>.pack for load-only runtime

Usage:
  python support/python/cook_shaders.py --shader-api d3d12
  python support/python/cook_shaders.py --shader-api both --force
"""

import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from build_wheel import BIN_DIR, write_shader_pack
from precompile_pt_shader_bins import run_pt_shader_precompile
from verify_pt_shader_bins import verify_apis


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Cook path-tracing shaders for load-only runtime: "
            "coverage precompile + verify + shader pack."
        )
    )
    parser.add_argument(
        "--shader-api",
        choices=["d3d12", "vulkan", "both"],
        default="d3d12" if os.name == "nt" else "vulkan",
    )
    parser.add_argument(
        "--global-preset",
        choices=["default", "coverage"],
        default="coverage",
        help="Closed feature-preset matrix. Release/distribution must use 'coverage'.",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Force recompilation even when bins already exist.",
    )
    parser.add_argument(
        "--skip-precompile",
        action="store_true",
        help="Skip DXC precompile (verify + pack only).",
    )
    parser.add_argument(
        "--skip-verify",
        action="store_true",
        help="Skip bin coverage verification (not recommended).",
    )
    parser.add_argument(
        "--skip-pack",
        action="store_true",
        help="Skip writing caustica.shaders.<api>.pack.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=BIN_DIR,
        help="Directory for caustica.shaders.<api>.pack (default: bin/).",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if args.global_preset != "coverage":
        print(
            "[caustica] WARNING: cooking with preset "
            f"'{args.global_preset}'. Release builds should use 'coverage'."
        )

    if not args.skip_precompile:
        print(f"[caustica] Cook step 1/3: precompile ({args.global_preset})")
        run_pt_shader_precompile(
            args.shader_api,
            force=args.force,
            global_preset=args.global_preset,
        )
    else:
        print("[caustica] Cook step 1/3: precompile skipped")

    if not args.skip_verify:
        print(f"[caustica] Cook step 2/3: verify ({args.global_preset})")
        rc = verify_apis(args.shader_api, args.global_preset)
        if rc != 0:
            return rc
    else:
        print("[caustica] Cook step 2/3: verify skipped")

    if not args.skip_pack:
        print("[caustica] Cook step 3/3: package shader packs")
        shader_types = (
            ["dxil"]
            if args.shader_api == "d3d12"
            else ["spirv"]
            if args.shader_api == "vulkan"
            else ["dxil", "spirv"]
        )
        args.output_dir.mkdir(parents=True, exist_ok=True)
        for shader_type in shader_types:
            pack_path = write_shader_pack(shader_type, "bin", args.output_dir)
            print(f"[caustica] wrote {pack_path}")
    else:
        print("[caustica] Cook step 3/3: pack skipped")

    print("[caustica] Cook complete.")
    print("  Runtime load-only expects caustica.shaders.<api>.pack next to the binary.")
    print("  Dev without pack can still use ShaderDynamic/Bin + optional runtime DXC.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

from __future__ import annotations

"""
Official Caustica PT cook entry point (UE-style offline specialization).

Two layers (do not confuse them):

  A) Shader libraries (this script, required for release):
     DXC-precompile the closed feature-preset matrix → ShaderDynamic/Bin
     → verify → package caustica.shaders.<api>.pack

  B) DXR state objects (CreateStateObject):
     Device-local, scene hit-group dependent. Not serialized as .bin files.
     Runtime builds only the *active* preset on first use.
     Optional GPU validation: --precache-rt-psos (headless CreateStateObject
     for every preset on the cook machine).

Usage:
  python support/python/cook_shaders.py --shader-api d3d12
  python support/python/cook_shaders.py --shader-api d3d12 --precache-rt-psos
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
            "Cook path-tracing shader libraries for load-only runtime: "
            "coverage precompile + verify + shader pack "
            "(+ optional GPU RT PSO precache validation)."
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
        "--precache-rt-psos",
        action="store_true",
        help=(
            "After packing, headless-CreateStateObject every feature preset "
            "(validates cooked bins link; does not persist PSOs to disk)."
        ),
    )
    parser.add_argument(
        "--precache-scene",
        default="builtin:plane_cube",
        help="Scene used when --precache-rt-psos is set.",
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

    step = 1
    total_steps = 3 + (1 if args.precache_rt_psos else 0)

    if not args.skip_precompile:
        print(f"[caustica] Cook step {step}/{total_steps}: precompile libraries ({args.global_preset})")
        run_pt_shader_precompile(
            args.shader_api,
            force=args.force,
            global_preset=args.global_preset,
        )
    else:
        print(f"[caustica] Cook step {step}/{total_steps}: precompile skipped")
    step += 1

    if not args.skip_verify:
        print(f"[caustica] Cook step {step}/{total_steps}: verify libraries ({args.global_preset})")
        rc = verify_apis(args.shader_api, args.global_preset)
        if rc != 0:
            return rc
    else:
        print(f"[caustica] Cook step {step}/{total_steps}: verify skipped")
    step += 1

    if not args.skip_pack:
        print(f"[caustica] Cook step {step}/{total_steps}: package shader packs")
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
        print(f"[caustica] Cook step {step}/{total_steps}: pack skipped")
    step += 1

    if args.precache_rt_psos:
        print(f"[caustica] Cook step {step}/{total_steps}: GPU precache RT PSOs")
        from precache_rt_presets import precache

        apis = (
            ["d3d12"]
            if args.shader_api == "d3d12"
            else ["vulkan"]
            if args.shader_api == "vulkan"
            else ["d3d12", "vulkan"]
        )
        for api in apis:
            rc = precache(shader_api=api, scene=args.precache_scene, frames_before=1)
            if rc != 0:
                return rc

    print("[caustica] Cook complete.")
    print("  Layer A (libraries): load-only uses ShaderDynamic/Bin or caustica.shaders.<api>.pack")
    print("  Layer B (RT PSOs): runtime CreateStateObject for active preset only;")
    print("                     use Renderer.precache_rt_feature_presets() at load if desired.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

from __future__ import annotations

import argparse
import os
import sys

from precompile_pt_shader_bins import (
    BIN_DIR,
    build_hash_command,
    build_jobs,
    cache_paths,
    hash_hex,
    runtime_bin_folder,
)


def verify(compile_api: str, global_preset: str = "coverage") -> tuple[int, list[str]]:
    missing: list[str] = []
    checked = 0
    folder = runtime_bin_folder(compile_api)
    for job in build_jobs(global_preset):
        digest = hash_hex(build_hash_command(job["logical"], job["macros"], api=compile_api))
        out_path, rel = cache_paths(compile_api, digest)
        checked += 1
        if not out_path.exists():
            missing.append(f"{job['label']}: ShaderDynamic/Bin/{folder}/{rel}")
    return checked, missing


def verify_apis(shader_api: str, global_preset: str = "coverage") -> int:
    apis = (
        ["d3d12"]
        if shader_api == "d3d12"
        else ["vulkan"]
        if shader_api == "vulkan"
        else ["d3d12", "vulkan"]
    )
    total_missing: list[str] = []
    total_checked = 0
    for api in apis:
        checked, missing = verify(api, global_preset)
        total_checked += checked
        folder = runtime_bin_folder(api)
        total_missing.extend(f"[{folder}] {item}" for item in missing)
        print(
            f"[caustica] PT shader verify ({folder}, preset={global_preset}): "
            f"checked={checked}, missing={len(missing)}"
        )

    if total_missing:
        print(
            f"[caustica] ERROR: {len(total_missing)} / {total_checked} cooked PT bins missing.",
            file=sys.stderr,
        )
        for item in total_missing[:32]:
            print(f"  - {item}", file=sys.stderr)
        if len(total_missing) > 32:
            print(f"  ... and {len(total_missing) - 32} more", file=sys.stderr)
        print(
            "Run: python support/python/cook_shaders.py --shader-api "
            f"{shader_api}",
            file=sys.stderr,
        )
        return 1

    print(
        f"[caustica] PT shader verify OK ({total_checked} bins under {BIN_DIR / 'ShaderDynamic' / 'Bin'})"
    )
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify offline path-tracing shader bins cover the closed feature-preset matrix."
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
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    return verify_apis(args.shader_api, args.global_preset)


if __name__ == "__main__":
    raise SystemExit(main())

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BIN_DIR = ROOT / "bin"
SHADER_ROOT = ROOT / "caustica" / "caustica" / "shaders"
INCLUDE_ROOT = ROOT / "caustica" / "caustica"
EXTERNAL_ROOT = ROOT / "External"

# Stable pipeline variants used at runtime (see SceneRayTracingResources.cpp).
PIPELINE_VARIANTS = [
    {
        "source": "PathTracerSample.hlsl",
        "pipeline_id": "REF",
        "macros": [("PATH_TRACER_MODE", "PATH_TRACER_MODE_REFERENCE")],
        "material_source": "PathTracerMaterialSpecializations.hlsl",
    },
    {
        "source": "PathTracerSample.hlsl",
        "pipeline_id": "BUILD",
        "macros": [("PATH_TRACER_MODE", "PATH_TRACER_MODE_BUILD_STABLE_PLANES")],
        "material_source": "PathTracerMaterialSpecializations.hlsl",
    },
    {
        "source": "PathTracerSample.hlsl",
        "pipeline_id": "FILL",
        "macros": [("PATH_TRACER_MODE", "PATH_TRACER_MODE_FILL_STABLE_PLANES")],
        "material_source": "PathTracerMaterialSpecializations.hlsl",
    },
]

TIER_STABLE_NAMES = [
    "Ubershader",
    "Standard",
    "NonEmissive",
    "Transmission",
    "ThinSurface",
    "NormalMap",
    "AlphaTest",
    "DeltaLobes",
]


def configure_import_path() -> None:
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(BIN_DIR))
    sys.path.insert(0, str(BIN_DIR))


def tier_macros(tier: int) -> list[tuple[str, str]]:
    macros = [
        ("CAUSTICA_MATERIAL_FEATURE_TIER", str(tier)),
    ]
    if tier == 0:
        macros.append(("CAUSTICA_MATERIAL_PERMUTATIONS_ENABLED", "0"))
        return macros

    macros.append(("CAUSTICA_MATERIAL_PERMUTATIONS_ENABLED", "1"))
    if tier == 2:
        macros.extend(
            [
                ("CAUSTICA_MATERIAL_IS_EMISSIVE", "0"),
                ("CAUSTICA_MATERIAL_IS_ANALYTIC_LIGHT_PROXY", "0"),
            ]
        )
    elif tier == 3:
        macros.append(("CAUSTICA_MATERIAL_HAS_TRANSMISSION", "1"))
    elif tier == 4:
        macros.append(("CAUSTICA_MATERIAL_THIN_SURFACE", "1"))
    elif tier == 5:
        macros.append(("CAUSTICA_MATERIAL_USE_NORMAL_TEXTURE", "1"))
    elif tier == 6:
        macros.append(("CAUSTICA_MATERIAL_ALPHA_TEST", "1"))
    elif tier == 7:
        macros.append(("CAUSTICA_MATERIAL_ONLY_DELTA_LOBES", "1"))
    return macros


def default_global_macros() -> list[tuple[str, str]]:
    # Reference-mode defaults with conservative feature flags for offline packs.
    return [
        ("ENABLE_DEBUG_SURFACE_VIZ", "0"),
        ("ENABLE_DEBUG_LINES_VIZ", "0"),
        ("USE_NVAPI_HIT_OBJECT_EXTENSION", "0"),
        ("USE_NVAPI_REORDER_THREADS", "0"),
        ("USE_DX_HIT_OBJECT_EXTENSION", "0"),
        ("USE_DX_MAYBE_REORDER_THREADS", "0"),
        ("PT_ENABLE_RUSSIAN_ROULETTE", "1"),
        ("PT_NEE_ENABLED", "1"),
        ("PT_USE_RESTIR_DI", "0"),
        ("PT_USE_RESTIR_GI", "0"),
        ("PT_USE_RESTIR_PT", "0"),
        ("CAUSTICA_ENABLE_OPACITY_MICROMAPS", "0"),
        ("CAUSTICA_USE_APPROXIMATE_MIS", "0"),
        ("CAUSTICA_NEE_FULL_SAMPLE_COUNT", "1"),
        ("CAUSTICA_NEE_LOCAL_CANDIDATE_SAMPLE_COUNT", "1"),
        ("CAUSTICA_NEE_GLOBAL_CANDIDATE_SAMPLE_COUNT", "0"),
        ("CAUSTICA_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT", "1"),
        ("CAUSTICA_DISABLE_SER_TERMINATION_HINT", "0"),
        ("CAUSTICA_DISCARD_NON_NEE_LIGHTING", "0"),
        ("CAUSTICA_DISCARD_NEE_LIGHTING", "0"),
        ("CAUSTICA_FIREFLY_FILTER", "0"),
        ("CAUSTICA_ACTIVE_STABLE_PLANE_COUNT", "0"),
        ("CAUSTICA_NESTED_DIELECTRICS_QUALITY", "0"),
        ("CAUSTICA_LP_TYPES_USE_16BIT_PRECISION", "0"),
        ("CAUSTICA_ENABLE_LOW_DISCREPANCY_SAMPLER_FOR_BSDF", "0"),
    ]


def vulkan_binding_shift_args() -> list[str]:
    # Matches nvrhi::VulkanBindingOffsets defaults used in ShaderCompilerUtils.cpp
    sampler, t, b, u = 128, 0, 256, 384
    args: list[str] = []
    for space in range(7):
        args.extend(
            [
                "-fvk-s-shift",
                str(sampler),
                str(space),
                "-fvk-t-shift",
                str(t),
                str(space),
                "-fvk-b-shift",
                str(b),
                str(space),
                "-fvk-u-shift",
                str(u),
                str(space),
            ]
        )
    return args


def build_hash_command(
    logical_source: str,
    macros: list[tuple[str, str]],
    *,
    api: str,
    profile: str = "lib_6_6",
) -> str:
    parts = [f' "{logical_source}"', " -Zi", " -Zsb", " -O3", " -enable-16bit-types", " -WX", " -all_resources_bound"]
    parts.append(f" -T {profile}")
    if profile.startswith("lib_6_6"):
        parts.append(" -enable-payload-qualifiers")
    parts.append(" -D ENABLE_DEBUG_PRINT")
    for name, definition in macros:
        parts.append(f" -D {name}={definition}")
    parts.extend([" -I <external1>", " -I <external2>"])
    if api == "d3d12":
        parts.append(" -D TARGET_D3D12")
    else:
        parts.append(" -D TARGET_VULKAN")
        parts.extend(
            [
                " -D SPIRV",
                " -spirv",
                " -fspv-target-env=vulkan1.2",
                " -fspv-extension=SPV_EXT_descriptor_indexing",
                " -fspv-extension=KHR",
            ]
        )
        for arg in vulkan_binding_shift_args():
            parts.append(f" {arg}")
    return "".join(parts)


def hash_hex(command: str) -> str:
    return hashlib.sha256(command.encode("utf-8")).hexdigest()


def cache_paths(api: str, digest: str) -> tuple[Path, str]:
    rel = f"{digest[:2]}/{digest}.bin"
    out_dir = BIN_DIR / "ShaderDynamic" / "Bin" / api / digest[:2]
    out_dir.mkdir(parents=True, exist_ok=True)
    return out_dir / f"{digest}.bin", rel


def find_dxc(api: str) -> Path:
    if api == "d3d12":
        candidates = [
            BIN_DIR / "ShaderDynamic" / "Tools" / "d3d12" / "x64" / "dxc.exe",
            os.environ.get("SHADERMAKE_DXC_PATH", ""),
        ]
    else:
        candidates = [
            BIN_DIR / "ShaderDynamic" / "Tools" / "vk" / "x64" / "dxc.exe",
            os.environ.get("SHADERMAKE_DXC_VK_PATH", ""),
            os.environ.get("DXC_SPIRV_PATH", ""),
        ]
    for candidate in candidates:
        if not candidate:
            continue
        path = Path(candidate)
        if path.exists():
            return path
    raise FileNotFoundError(f"DXC not found for API {api}")


def compile_library(
    dxc: Path,
    *,
    source: Path,
    logical_source: str,
    macros: list[tuple[str, str]],
    api: str,
    profile: str = "lib_6_6",
    force: bool = False,
) -> Path | None:
    digest = hash_hex(build_hash_command(logical_source, macros, api=api, profile=profile))
    out_path, _ = cache_paths(api, digest)
    if out_path.exists() and not force:
        return out_path

    cmd = [str(dxc), str(source), "-Zi", "-Zsb", "-O3", "-enable-16bit-types", "-WX", "-all_resources_bound", "-T", profile]
    if profile.startswith("lib_6_6"):
        cmd.append("-enable-payload-qualifiers")
    cmd.extend(["-D", "ENABLE_DEBUG_PRINT"])
    for name, definition in macros:
        cmd.extend(["-D", f"{name}={definition}"])
    cmd.extend(["-I", str(INCLUDE_ROOT)])
    cmd.extend(["-I", str(EXTERNAL_ROOT)])
    if api == "d3d12":
        cmd.extend(["-D", "TARGET_D3D12"])
    else:
        cmd.extend(
            [
                "-D",
                "TARGET_VULKAN",
                "-D",
                "SPIRV",
                "-spirv",
                "-fspv-target-env=vulkan1.2",
                "-fspv-extension=SPV_EXT_descriptor_indexing",
                "-fspv-extension=KHR",
            ]
        )
        cmd.extend(vulkan_binding_shift_args())
    cmd.extend(["-Fo", str(out_path)])

    print(f"[caustica] DXC {logical_source} -> {out_path.name}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        raise RuntimeError(f"DXC failed for {logical_source}")
    return out_path


def build_jobs() -> list[dict]:
    jobs: list[dict] = []
    global_macros = default_global_macros()

    for variant in PIPELINE_VARIANTS:
        pipeline_id = variant["pipeline_id"]
        pipeline_macros = list(global_macros)
        pipeline_macros.extend(variant["macros"])
        pipeline_macros.append(("CAUSTICA_PIPELINE_PERMUTATION_NAME", pipeline_id))

        jobs.append(
            {
                "source": SHADER_ROOT / variant["source"],
                "logical": variant["source"],
                "macros": list(pipeline_macros),
                "label": f"{pipeline_id}_raygen",
            }
        )

        material_source = variant["material_source"]
        for tier, stable_name in enumerate(TIER_STABLE_NAMES):
            permutation_name = f"{pipeline_id}_{stable_name}"
            material_macros = list(pipeline_macros)
            material_macros.extend(tier_macros(tier))
            material_macros.append(("CAUSTICA_MATERIAL_PERMUTATION_NAME", permutation_name))
            shader_id = "-1" if tier == 0 else str(tier)
            material_macros.append(("CAUSTICA_SHADER_ID", shader_id))
            jobs.append(
                {
                    "source": SHADER_ROOT / material_source,
                    "logical": material_source,
                    "macros": material_macros,
                    "label": permutation_name,
                }
            )
    return jobs


def precompile(api: str, force: bool) -> int:
    dxc = find_dxc(api)
    compiled = 0
    skipped = 0
    for job in build_jobs():
        digest = hash_hex(build_hash_command(job["logical"], job["macros"], api=api))
        out_path, _ = cache_paths(api, digest)
        if out_path.exists() and not force:
            skipped += 1
            continue
        compile_library(
            dxc,
            source=job["source"],
            logical_source=job["logical"],
            macros=job["macros"],
            api=api,
            force=force,
        )
        compiled += 1
    print(f"[caustica] PT shader precompile ({api}): compiled={compiled}, skipped={skipped}")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Precompile path-tracing shader libraries to ShaderDynamic/Bin using DXC (hash-compatible with runtime)."
    )
    parser.add_argument(
        "--shader-api",
        choices=["d3d12", "vulkan", "both"],
        default="d3d12" if os.name == "nt" else "vulkan",
    )
    parser.add_argument("--force", action="store_true", help="Recompile even if output bins already exist.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    apis = ["dxil"] if args.shader_api == "d3d12" else ["spirv"] if args.shader_api == "vulkan" else ["dxil", "spirv"]
    api_map = {"dxil": "d3d12", "spirv": "vulkan"}
    for api_folder in apis:
        precompile(api_map[api_folder], args.force)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

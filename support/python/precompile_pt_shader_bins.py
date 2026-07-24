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
    {
        "source": "TestRaygenPP.hlsl",
        "pipeline_id": "TESTRG",
        "macros": [("PP_TEST_HDR", "1")],
        "material_source": None,
    },
    {
        "source": "TestRaygenPP.hlsl",
        "pipeline_id": "EDGY",
        "macros": [("PP_EDGE_DETECTION", "1")],
        "material_source": None,
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

# Matches ComputeCandidateSampleLocalCount(0.65, 5) in LightingTypes.hlsli.
DEFAULT_NEE_LOCAL_CANDIDATES = 3
DEFAULT_NEE_GLOBAL_CANDIDATES = 2
DEFAULT_NEE_TOTAL_CANDIDATES = 5
DEFAULT_STABLE_PLANE_COUNT = 3


def configure_import_path() -> None:
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(BIN_DIR))
    sys.path.insert(0, str(BIN_DIR))


# Keep in sync with sizeof(StandardMaterialData) in
# caustica/shaders/PathTracer/Materials/StandardMaterial.h (17 x 16 bytes).
STANDARD_MATERIAL_DATA_BYTES = "272"


def tier_macros(tier: int) -> list[tuple[str, str]]:
    macros = [
        # Include after rebuilding caustica.exe that emits the same macro.
        # ("CAUSTICA_STANDARD_MATERIAL_DATA_BYTES", STANDARD_MATERIAL_DATA_BYTES),
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


def base_global_macro_map() -> dict[str, str]:
    # Keep in sync with PtPipelineFeaturePresets.cpp::fillBaseMacros defaults.
    # Omit CAUSTICA_STANDARD_MATERIAL_DATA_BYTES until the matching exe is rebuilt,
    # so --force can overwrite the pre-OpenPBR ClosestHit bins still referenced at runtime.
    return {
        # "CAUSTICA_STANDARD_MATERIAL_DATA_BYTES": STANDARD_MATERIAL_DATA_BYTES,
        "ENABLE_DEBUG_SURFACE_VIZ": "0",
        "ENABLE_DEBUG_LINES_VIZ": "0",
        "USE_NVAPI_HIT_OBJECT_EXTENSION": "0",
        "USE_NVAPI_REORDER_THREADS": "0",
        "USE_DX_HIT_OBJECT_EXTENSION": "0",
        "USE_DX_MAYBE_REORDER_THREADS": "0",
        "PT_ENABLE_RUSSIAN_ROULETTE": "1",
        "PT_NEE_ENABLED": "1",
        "PT_USE_RESTIR_DI": "0",
        "PT_USE_RESTIR_GI": "0",
        "PT_USE_RESTIR_PT": "0",
        "CAUSTICA_ENABLE_OPACITY_MICROMAPS": "0",
        "CAUSTICA_USE_APPROXIMATE_MIS": "1",
        "CAUSTICA_NEE_FULL_SAMPLE_COUNT": "1",
        "CAUSTICA_NEE_LOCAL_CANDIDATE_SAMPLE_COUNT": str(DEFAULT_NEE_LOCAL_CANDIDATES),
        "CAUSTICA_NEE_GLOBAL_CANDIDATE_SAMPLE_COUNT": str(DEFAULT_NEE_GLOBAL_CANDIDATES),
        "CAUSTICA_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT": str(DEFAULT_NEE_TOTAL_CANDIDATES),
        "CAUSTICA_DISABLE_SER_TERMINATION_HINT": "0",
        "CAUSTICA_DISCARD_NON_NEE_LIGHTING": "0",
        "CAUSTICA_DISCARD_NEE_LIGHTING": "0",
        "CAUSTICA_FIREFLY_FILTER": "1",
        "CAUSTICA_ACTIVE_STABLE_PLANE_COUNT": str(DEFAULT_STABLE_PLANE_COUNT),
        "CAUSTICA_NESTED_DIELECTRICS_QUALITY": "1",
        "CAUSTICA_LP_TYPES_USE_16BIT_PRECISION": "1",
        "CAUSTICA_ENABLE_LOW_DISCREPANCY_SAMPLER_FOR_BSDF": "1",
        "NEE_AT_SAMPLE_BAKED_ENVIRONMENT": "0",
    }


def macro_map_to_list(values: dict[str, str]) -> list[tuple[str, str]]:
    # Runtime hashing preserves macro insertion order. Keep this in sync with
    # SceneRayTracingResources::fillPTPipelineGlobalMacros.
    return list(values.items())


# Keep names/order in sync with caustica::render::PtFeaturePresetId
# (PtPipelineFeaturePresets.h) and fillPtFeaturePresetMacros().
# Curated combos only — do not expand into a full cartesian product.
COVERAGE_PRESET_OVERRIDES: list[tuple[str, dict[str, str]]] = [
    # Single-axis
    ("Default", {}),
    ("ReSTIR_DI", {"PT_USE_RESTIR_DI": "1"}),
    ("ReSTIR_GI", {"PT_USE_RESTIR_GI": "1"}),
    ("ReSTIR_PT", {"PT_USE_RESTIR_PT": "1"}),
    ("OMM_On", {"CAUSTICA_ENABLE_OPACITY_MICROMAPS": "1"}),
    ("NEE_Off", {"PT_NEE_ENABLED": "0"}),
    ("RR_Off", {"PT_ENABLE_RUSSIAN_ROULETTE": "0"}),
    ("Fp32Types", {"CAUSTICA_LP_TYPES_USE_16BIT_PRECISION": "0"}),
    ("LD_Off", {"CAUSTICA_ENABLE_LOW_DISCREPANCY_SAMPLER_FOR_BSDF": "0"}),
    ("Firefly_Off", {"CAUSTICA_FIREFLY_FILTER": "0"}),
    ("ApproxMIS_Off", {"CAUSTICA_USE_APPROXIMATE_MIS": "0"}),
    ("BakedEnv_On", {"NEE_AT_SAMPLE_BAKED_ENVIRONMENT": "1"}),
    ("NEE_Off_BakedEnv", {"PT_NEE_ENABLED": "0", "NEE_AT_SAMPLE_BAKED_ENVIRONMENT": "1"}),
    (
        "NEE_Candidates_8",
        {
            "CAUSTICA_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT": "8",
            "CAUSTICA_NEE_LOCAL_CANDIDATE_SAMPLE_COUNT": "5",
            "CAUSTICA_NEE_GLOBAL_CANDIDATE_SAMPLE_COUNT": "3",
        },
    ),
    ("StablePlanes_1", {"CAUSTICA_ACTIVE_STABLE_PLANE_COUNT": "1"}),
    ("NestedQuality_2", {"CAUSTICA_NESTED_DIELECTRICS_QUALITY": "2"}),
    # Curated multi-feature combos (common editor / realtime paths)
    (
        "ReSTIR_DI_OMM",
        {"PT_USE_RESTIR_DI": "1", "CAUSTICA_ENABLE_OPACITY_MICROMAPS": "1"},
    ),
    (
        "ReSTIR_GI_OMM",
        {"PT_USE_RESTIR_GI": "1", "CAUSTICA_ENABLE_OPACITY_MICROMAPS": "1"},
    ),
    (
        "ReSTIR_PT_OMM",
        {"PT_USE_RESTIR_PT": "1", "CAUSTICA_ENABLE_OPACITY_MICROMAPS": "1"},
    ),
    (
        "ReSTIR_DI_BakedEnv",
        {"PT_USE_RESTIR_DI": "1", "NEE_AT_SAMPLE_BAKED_ENVIRONMENT": "1"},
    ),
    (
        "ReSTIR_GI_BakedEnv",
        {"PT_USE_RESTIR_GI": "1", "NEE_AT_SAMPLE_BAKED_ENVIRONMENT": "1"},
    ),
    (
        "ReSTIR_PT_BakedEnv",
        {"PT_USE_RESTIR_PT": "1", "NEE_AT_SAMPLE_BAKED_ENVIRONMENT": "1"},
    ),
    (
        "ReSTIR_DI_OMM_BakedEnv",
        {
            "PT_USE_RESTIR_DI": "1",
            "CAUSTICA_ENABLE_OPACITY_MICROMAPS": "1",
            "NEE_AT_SAMPLE_BAKED_ENVIRONMENT": "1",
        },
    ),
    (
        "OMM_BakedEnv",
        {
            "CAUSTICA_ENABLE_OPACITY_MICROMAPS": "1",
            "NEE_AT_SAMPLE_BAKED_ENVIRONMENT": "1",
        },
    ),
    (
        "ReSTIR_DI_NEE8",
        {
            "PT_USE_RESTIR_DI": "1",
            "CAUSTICA_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT": "8",
            "CAUSTICA_NEE_LOCAL_CANDIDATE_SAMPLE_COUNT": "5",
            "CAUSTICA_NEE_GLOBAL_CANDIDATE_SAMPLE_COUNT": "3",
        },
    ),
    (
        "ReSTIR_DI_StablePlanes_1",
        {"PT_USE_RESTIR_DI": "1", "CAUSTICA_ACTIVE_STABLE_PLANE_COUNT": "1"},
    ),
]


def global_macro_presets(preset: str) -> list[list[tuple[str, str]]]:
    base = base_global_macro_map()
    if preset == "default":
        return [macro_map_to_list(base)]

    if preset != "coverage":
        raise ValueError(f"Unknown global macro preset: {preset}")

    presets: list[list[tuple[str, str]]] = []
    seen: set[tuple[tuple[str, str], ...]] = set()
    for name, overrides in COVERAGE_PRESET_OVERRIDES:
        merged = {**base, **overrides}
        macro_list = macro_map_to_list(merged)
        key = tuple(macro_list)
        if key in seen:
            continue
        seen.add(key)
        print(f"[caustica] PT feature preset: {name}")
        presets.append(macro_list)
    return presets


def vulkan_binding_shift_args() -> list[str]:
    # Matches caustica::rhi::VulkanBindingOffsets defaults used in ShaderCompilerUtils.cpp
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


# Compile API names (d3d12/vulkan) vs runtime bin folders (dxil/spirv).
# Must match caustica::getShaderTypeName() / ShaderCompilerConfig::ShaderBinariesPath.
RUNTIME_BIN_FOLDER = {
    "d3d12": "dxil",
    "vulkan": "spirv",
}


def runtime_bin_folder(compile_api: str) -> str:
    try:
        return RUNTIME_BIN_FOLDER[compile_api]
    except KeyError as exc:
        raise ValueError(f"Unsupported compile API '{compile_api}'") from exc


def cache_paths(compile_api: str, digest: str) -> tuple[Path, str]:
    # Match ShaderKey::formatCacheFileNameNoExt: split the first two hex chars
    # into the directory and store only the remaining suffix as the file name.
    # Folder must be the runtime type name (dxil/spirv), not the cook CLI name.
    folder = runtime_bin_folder(compile_api)
    file_stem = digest[2:] if len(digest) >= 2 else digest
    rel = f"{digest[:2]}/{file_stem}.bin"
    out_dir = BIN_DIR / "ShaderDynamic" / "Bin" / folder / digest[:2]
    out_dir.mkdir(parents=True, exist_ok=True)
    return out_dir / f"{file_stem}.bin", rel


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


def build_jobs(global_preset: str) -> list[dict]:
    jobs: list[dict] = []

    for global_macros in global_macro_presets(global_preset):
        for variant in PIPELINE_VARIANTS:
            pipeline_id = variant["pipeline_id"]
            # Match PathTracingShaderCompiler: variant macros + baked globals.
            # CAUSTICA_PIPELINE_PERMUTATION_NAME is raygen-only (not on hit materials).
            pipeline_macros = list(variant["macros"])
            pipeline_macros.extend(global_macros)

            raygen_macros = list(pipeline_macros)
            raygen_macros.append(("CAUSTICA_PIPELINE_PERMUTATION_NAME", pipeline_id))
            jobs.append(
                {
                    "source": SHADER_ROOT / variant["source"],
                    "logical": variant["source"],
                    "macros": raygen_macros,
                    "label": f"{pipeline_id}_raygen",
                }
            )

            material_source = variant.get("material_source")
            if not material_source:
                continue

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


def precompile(api: str, force: bool, global_preset: str = "default") -> int:
    dxc = find_dxc(api)
    compiled = 0
    skipped = 0
    for job in build_jobs(global_preset):
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
    print(
        f"[caustica] PT shader precompile ({api}, preset={global_preset}): "
        f"compiled={compiled}, skipped={skipped}"
    )
    return 0


def run_pt_shader_precompile(
    shader_api: str,
    *,
    force: bool = False,
    global_preset: str = "coverage",
) -> None:
    compile_apis = (
        ["d3d12"]
        if shader_api == "d3d12"
        else ["vulkan"]
        if shader_api == "vulkan"
        else ["d3d12", "vulkan"]
    )
    for compile_api in compile_apis:
        precompile(compile_api, force, global_preset)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Precompile path-tracing shader libraries to ShaderDynamic/Bin using DXC (hash-compatible with runtime)."
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
        help=(
            "Closed feature-preset matrix to precompile. "
            "'coverage' is required for UE-style load-only runtime switching."
        ),
    )
    parser.add_argument("--force", action="store_true", help="Recompile even if output bins already exist.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    run_pt_shader_precompile(args.shader_api, force=args.force, global_preset=args.global_preset)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

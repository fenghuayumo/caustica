from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from shader_cook_cache import (
    CookStats,
    DependencyManifest,
    ShaderDdc,
    collect_pdbs,
    compute_l2_key,
    preprocess,
    shader_relative,
)


ROOT = Path(__file__).resolve().parents[2]
BIN_DIR = ROOT / "bin"
SHADER_ROOT = ROOT / "caustica" / "caustica" / "shaders"
INCLUDE_ROOT = ROOT / "caustica" / "caustica"
EXTERNAL_ROOT = ROOT / "External"

# L2 (content-addressed) compile cache and the external PDBs that go with it. Kept
# out of bin/ because bin/ is the shipped runtime layout: these are build
# intermediates, and only ShaderBin (plus the .pack) is meant to be distributed.
# Point CAUSTICA_SHADER_DDC at a share to let a team reuse CI's compiles.
DDC_DIR = Path(os.environ.get("CAUSTICA_SHADER_DDC", ROOT / ".shadercache"))

# Stable pipeline variants used at runtime (see SceneRayTracingResources.cpp).
PIPELINE_VARIANTS = [
    {
        "source": "PathTracerEntryPoint.hlsl",
        "pipeline_id": "REF",
        "macros": [("PATH_TRACER_MODE", "PATH_TRACER_MODE_REFERENCE")],
        "material_source": "PathTracerMaterialSpecializations.hlsl",
    },
    {
        "source": "PathTracerEntryPoint.hlsl",
        "pipeline_id": "BUILD",
        "macros": [("PATH_TRACER_MODE", "PATH_TRACER_MODE_BUILD_STABLE_PLANES")],
        "material_source": "PathTracerMaterialSpecializations.hlsl",
    },
    {
        "source": "PathTracerEntryPoint.hlsl",
        "pipeline_id": "FILL",
        "macros": [("PATH_TRACER_MODE", "PATH_TRACER_MODE_FILL_STABLE_PLANES")],
        "material_source": "PathTracerMaterialSpecializations.hlsl",
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
# caustica/shaders/PathTracer/Materials/StandardMaterial.h (22 x 16 bytes).
STANDARD_MATERIAL_DATA_BYTES = "352"


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

    for name, overrides in COVERAGE_PRESET_OVERRIDES:
        if preset == name:
            merged = {**base, **overrides}
            print(f"[caustica] PT feature preset: {name}")
            return [macro_map_to_list(merged)]

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
    debug_info: bool = True,
) -> str:
    # L1 key. Must stay byte-identical to ShaderCompilerUtils::buildDxcCommand()'s
    # hashCommand or the runtime will look for bins the cook never wrote. Note that
    # -Fo/-Fd are appended downstream and are deliberately outside the hash, which
    # is what lets the cook switch to external PDBs without invalidating anything.
    parts = [f' "{logical_source}"']
    if debug_info:
        parts.append(" -Zi")
    parts += [" -Zsb", " -O3", " -enable-16bit-types", " -WX", " -all_resources_bound"]
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


# The argument split below is what makes L2 dedup sound. `codegen_flags` holds every
# switch that changes output but is invisible to the preprocessor; macros and include
# dirs are excluded because their whole effect is already captured by hashing the
# preprocessed text. Getting this split wrong in either direction is a correctness
# bug: too narrow and distinct outputs collide, too wide and dedup stops firing.
def codegen_flags(api: str, profile: str = "lib_6_6", *, debug_info: bool = True) -> list[str]:
    flags = ["-Zsb", "-O3", "-enable-16bit-types", "-WX", "-all_resources_bound", "-T", profile]
    if debug_info:
        flags.insert(0, "-Zi")
    if profile.startswith("lib_6_6"):
        flags.append("-enable-payload-qualifiers")
    if api != "d3d12":
        flags += [
            "-spirv",
            "-fspv-target-env=vulkan1.2",
            "-fspv-extension=SPV_EXT_descriptor_indexing",
            "-fspv-extension=KHR",
            *vulkan_binding_shift_args(),
        ]
    return flags


def macro_flags(macros: list[tuple[str, str]], api: str) -> list[str]:
    flags = ["-D", "ENABLE_DEBUG_PRINT"]
    for name, definition in macros:
        flags += ["-D", f"{name}={definition}"]
    if api == "d3d12":
        flags += ["-D", "TARGET_D3D12"]
    else:
        flags += ["-D", "TARGET_VULKAN", "-D", "SPIRV"]
    return flags


def include_flags() -> list[str]:
    return ["-I", str(INCLUDE_ROOT), "-I", str(EXTERNAL_ROOT)]


def flag_signature(api: str, profile: str = "lib_6_6", *, debug_info: bool = True) -> str:
    return " ".join(codegen_flags(api, profile, debug_info=debug_info))


def emits_external_pdb(api: str) -> bool:
    """DXIL supports `/Fd`; SPIR-V carries debug info inline and rejects it.

    Mirrors the runtime choice in PathTracingShaderCompiler::resolveCacheIdentity,
    which already writes external PDBs for D3D12 and none for Vulkan.
    """
    return api == "d3d12"


def pdb_root(api: str) -> Path:
    """Shared PDB directory. Add it to the debugger's symbol search path.

    DXC names each PDB after the shader hash, which is exactly what PIX looks up,
    so one directory serves every cooked shader and duplicates collapse on their own.
    """
    root = DDC_DIR / "pdb" / runtime_bin_folder(api)
    root.mkdir(parents=True, exist_ok=True)
    return root


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


def cache_paths(compile_api: str, digest: str, *, create: bool = True) -> tuple[Path, str]:
    # Match ShaderKey::formatCacheFileNameNoExt: split the first two hex chars
    # into the directory and store only the remaining suffix as the file name.
    # Folder must be the runtime type name (dxil/spirv), not the cook CLI name.
    folder = runtime_bin_folder(compile_api)
    file_stem = digest[2:] if len(digest) >= 2 else digest
    rel = f"{digest[:2]}/{file_stem}.bin"
    out_dir = BIN_DIR / "ShaderBin" / folder / digest[:2]
    if create:
        out_dir.mkdir(parents=True, exist_ok=True)
    return out_dir / f"{file_stem}.bin", rel


def find_dxc(api: str) -> Path:
    if api == "d3d12":
        candidates = [
            BIN_DIR / "ShaderDev" / "Tools" / "d3d12" / "x64" / "dxc.exe",
            os.environ.get("SHADERMAKE_DXC_PATH", ""),
        ]
    else:
        candidates = [
            BIN_DIR / "ShaderDev" / "Tools" / "vk" / "x64" / "dxc",
            BIN_DIR / "ShaderDev" / "Tools" / "vk" / "x64" / "dxc.exe",
            os.environ.get("SHADERMAKE_DXC_VK_PATH", ""),
            os.environ.get("DXC_SPIRV_PATH", ""),
            os.environ.get("VULKAN_SDK", "") and str(Path(os.environ["VULKAN_SDK"]) / "bin" / "dxc"),
        ]
    for candidate in candidates:
        if not candidate:
            continue
        path = Path(candidate)
        if path.exists():
            return path
    raise FileNotFoundError(f"DXC not found for API {api}")


@dataclass
class CookJob:
    api: str
    source: Path
    logical: str
    macros: list[tuple[str, str]]
    label: str
    profile: str = "lib_6_6"
    l1_digest: str = ""
    l1_path: Path = field(default_factory=Path)
    l2_key: str = ""
    closure: tuple[str, ...] = ()

    @property
    def source_relpath(self) -> str:
        # Matches ShaderPermutation::shaderSrcFileName, which is how the runtime
        # identifies the root source it is resolving.
        return self.source.resolve().relative_to(SHADER_ROOT).as_posix()


def plan_jobs(apis: list[str], global_preset: str, *, debug_info: bool = True) -> list[CookJob]:
    """Expand the preset matrix across every requested API into one flat job list.

    Both APIs share a single pool so a d3d12/vulkan cook saturates the machine
    instead of draining one API's tail before starting the next.
    """
    planned: list[CookJob] = []
    for api in apis:
        for job in build_jobs(global_preset):
            digest = hash_hex(
                build_hash_command(
                    job["logical"], job["macros"], api=api, debug_info=debug_info
                )
            )
            l1_path, _ = cache_paths(api, digest, create=False)
            planned.append(
                CookJob(
                    api=api,
                    source=job["source"],
                    logical=job["logical"],
                    macros=job["macros"],
                    label=job["label"],
                    l1_digest=digest,
                    l1_path=l1_path,
                )
            )
    return planned


def compile_to_ddc(
    dxc: Path,
    ddc: ShaderDdc,
    *,
    job: CookJob,
    debug_info: bool = True,
) -> None:
    """Compile one distinct L2 entry and publish it to the content-addressed store.

    Output goes to a scratch directory first so a failed or interrupted DXC run can
    never publish a partial blob that a later cook would treat as a cache hit.
    """
    external_pdb = debug_info and emits_external_pdb(job.api)
    with tempfile.TemporaryDirectory(prefix="caus_cc_") as tmp:
        blob = Path(tmp) / "out.bin"
        cmd = [
            str(dxc), str(job.source),
            *codegen_flags(job.api, job.profile, debug_info=debug_info),
            *macro_flags(job.macros, job.api),
            *include_flags(),
        ]
        pdb_scratch = Path(tmp) / "pdb"
        if external_pdb:
            pdb_scratch.mkdir()
            # Trailing separator is load-bearing: it tells DXC to auto-name the PDB
            # after the shader hash instead of embedding a caller-supplied filename,
            # which would otherwise leak the scratch path into the container and
            # make otherwise-identical variants differ byte-wise.
            cmd += ["-Fd", str(pdb_scratch) + os.sep]
        cmd += ["-Fo", str(blob)]

        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0 or not blob.is_file():
            detail = (result.stderr or result.stdout or "").strip()
            raise RuntimeError(f"DXC failed for {job.logical} [{job.label}]: {detail}")

        ddc.publish(job.l2_key, blob)
        if external_pdb:
            collect_pdbs(pdb_scratch, pdb_root(job.api))


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
    """Compile a single library straight to its L1 path, bypassing the L2 cache.

    Retained for callers that want a one-off compile; the cook itself goes through
    `cook()` so it gets dedup and content-addressed caching.
    """
    digest = hash_hex(build_hash_command(logical_source, macros, api=api, profile=profile))
    out_path, _ = cache_paths(api, digest)

    cmd = [str(dxc), str(source), *codegen_flags(api, profile),
           *macro_flags(macros, api), *include_flags()]
    if emits_external_pdb(api):
        cmd += ["-Fd", str(pdb_root(api)) + os.sep]
    cmd += ["-Fo", str(out_path)]

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


def worker_count(pending: int) -> int:
    """DXC runs out-of-process, so the cook scales with cores rather than the GIL.

    Measured scaling on a 32-thread box: 4 workers 3.4x, 8 workers 6.0x, 16 workers
    8.4x, 32 workers 9.1x. Past 16 the curve is flat — the limit is memory bandwidth,
    not core count — so the remaining ~8% is not worth another 16 concurrent DXC
    working sets. Override with CAUSTICA_PT_SHADER_JOBS to retune per machine.
    """
    requested = os.environ.get("CAUSTICA_PT_SHADER_JOBS")
    default = min(16, max(2, os.cpu_count() or 4))
    count = int(requested) if requested else default
    return max(1, min(count, pending or 1))


def _guarded(fn):
    """Let a whole parallel phase finish so every failure is reported at once."""

    def wrapper(item):
        try:
            return fn(item)
        except Exception as exc:  # noqa: BLE001 - surfaced via CookStats.errors
            return exc

    return wrapper


def _resolve_l2_keys(
    dxc_for_api: dict[str, Path],
    jobs: list[CookJob],
    stats: CookStats,
    manifests: dict[str, DependencyManifest],
    *,
    debug_info: bool,
) -> list[CookJob]:
    """Preprocess every job to derive its content-based L2 key and include closure.

    This replaces the old directory-mtime check. Preprocessing is ~100x cheaper than
    compiling, so paying it unconditionally buys exact invalidation: an edit that
    does not change a variant's preprocessed text leaves that variant's key — and
    therefore its cached blob — untouched.
    """
    signatures = {api: flag_signature(api, debug_info=debug_info) for api in dxc_for_api}

    def resolve(job: CookJob) -> CookJob:
        result = preprocess(
            dxc_for_api[job.api],
            source=job.source,
            macro_args=macro_flags(job.macros, job.api),
            include_args=include_flags(),
            profile_args=["-T", job.profile]
            + (["-enable-payload-qualifiers"] if job.profile.startswith("lib_6_6") else []),
        )
        job.l2_key = compute_l2_key(
            api=job.api,
            flag_signature=signatures[job.api],
            source_sha=result.source_sha,
        )
        job.closure = tuple(
            shader_relative(path, SHADER_ROOT, ROOT) for path in result.includes
        )
        return job

    resolved: list[CookJob] = []
    with ThreadPoolExecutor(max_workers=worker_count(len(jobs))) as executor:
        for job, outcome in zip(jobs, executor.map(_guarded(resolve), jobs)):
            if isinstance(outcome, Exception):
                stats.errors.append((job.label, str(outcome)))
                continue
            resolved.append(outcome)
            stats.preprocessed += 1
            manifests[outcome.api].add_closure(outcome.source_relpath, outcome.closure)
    return resolved


def _report_errors(stats: CookStats) -> int:
    print(f"[caustica] ERROR: {len(stats.errors)} shader(s) failed:", file=sys.stderr)
    for label, detail in stats.errors[:16]:
        print(f"  - {label}: {detail}", file=sys.stderr)
    if len(stats.errors) > 16:
        print(f"  ... and {len(stats.errors) - 16} more", file=sys.stderr)
    raise RuntimeError(f"{len(stats.errors)} shader(s) failed to cook")


def cook(
    apis: list[str],
    *,
    force: bool = False,
    global_preset: str = "coverage",
    debug_info: bool = True,
) -> int:
    dxc_for_api = {api: find_dxc(api) for api in apis}
    ddc = ShaderDdc(DDC_DIR)
    stats = CookStats()

    jobs = plan_jobs(apis, global_preset, debug_info=debug_info)
    stats.jobs = len(jobs)
    label = "+".join(apis)
    print(
        f"[caustica] PT shader cook ({label}, preset={global_preset}): "
        f"{stats.jobs} variants, {worker_count(stats.jobs)} parallel jobs"
    )

    manifests = {api: DependencyManifest() for api in apis}
    started = time.perf_counter()
    jobs = _resolve_l2_keys(dxc_for_api, jobs, stats, manifests, debug_info=debug_info)
    preprocess_seconds = time.perf_counter() - started
    if stats.errors:
        return _report_errors(stats)

    distinct: dict[tuple[str, str], CookJob] = {}
    for job in jobs:
        distinct.setdefault((job.api, job.l2_key), job)
    stats.distinct = len(distinct)
    root_sources = {src for m in manifests.values() for src in m.closures}
    print(
        f"[caustica] preprocessed {stats.preprocessed} variants in "
        f"{preprocess_seconds:.1f}s over {len(root_sources)} root sources; "
        f"{stats.distinct} distinct compiles ({stats.deduped} deduplicated)"
    )

    pending = [job for job in distinct.values() if force or not ddc.has(job.l2_key)]
    stats.ddc_hits = stats.distinct - len(pending)
    print(f"[caustica] compile cache: {stats.ddc_hits} hits, {len(pending)} to compile")

    if pending:
        started = time.perf_counter()
        completed = 0
        with ThreadPoolExecutor(max_workers=worker_count(len(pending))) as executor:
            work = _guarded(
                lambda job: compile_to_ddc(
                    dxc_for_api[job.api], ddc, job=job, debug_info=debug_info
                )
            )
            for job, outcome in zip(pending, executor.map(work, pending)):
                if isinstance(outcome, Exception):
                    stats.errors.append((job.label, str(outcome)))
                    continue
                stats.compiled += 1
                completed += 1
                if completed % 25 == 0 or completed == len(pending):
                    print(f"[caustica]   compiled {completed}/{len(pending)}")
        print(f"[caustica] compiled {stats.compiled} libraries in "
              f"{time.perf_counter() - started:.1f}s")

    if stats.errors:
        return _report_errors(stats)

    fingerprints = {
        api: {
            source: manifest.fingerprint(source, SHADER_ROOT)
            for source in manifest.closures
        }
        for api, manifest in manifests.items()
    }
    for job in jobs:
        if ddc.materialize(job.l2_key, job.l1_path):
            stats.l1_written += 1
        else:
            stats.l1_reused += 1
        manifests[job.api].add_bin(
            job.l1_digest, job.source_relpath, fingerprints[job.api][job.source_relpath]
        )

    for api, manifest in manifests.items():
        manifest.write(BIN_DIR / "ShaderBin" / runtime_bin_folder(api) / "deps.manifest")

    print(
        f"[caustica] PT shader cook ({label}): compiled={stats.compiled}, "
        f"cache_hits={stats.ddc_hits}, deduplicated={stats.deduped}, "
        f"bins_written={stats.l1_written}, bins_unchanged={stats.l1_reused}"
    )
    return 0


def precompile(
    api: str,
    force: bool,
    global_preset: str = "default",
    *,
    debug_info: bool = True,
) -> int:
    return cook([api], force=force, global_preset=global_preset, debug_info=debug_info)


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
    cook(compile_apis, force=force, global_preset=global_preset)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Precompile path-tracing shader libraries to ShaderBin using DXC (hash-compatible with runtime)."
    )
    parser.add_argument(
        "--shader-api",
        choices=["d3d12", "vulkan", "both"],
        default="d3d12" if os.name == "nt" else "vulkan",
    )
    parser.add_argument(
        "--global-preset",
        choices=["default", "coverage", *(name for name, _ in COVERAGE_PRESET_OVERRIDES)],
        default="coverage",
        help=(
            "Closed feature-preset matrix to precompile. "
            "'coverage' is required for UE-style load-only runtime switching."
        ),
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Recompile every variant, ignoring the content-addressed compile cache.",
    )
    parser.add_argument(
        "--no-debug-info",
        action="store_true",
        help=(
            "Drop -Zi entirely: ~30%% faster per compile and no PDBs, at the cost of "
            "shader debugging. Changes the L1 hash, so these bins are not "
            "interchangeable with a debug cook."
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    apis = (
        ["d3d12"]
        if args.shader_api == "d3d12"
        else ["vulkan"]
        if args.shader_api == "vulkan"
        else ["d3d12", "vulkan"]
    )
    return cook(
        apis,
        force=args.force,
        global_preset=args.global_preset,
        debug_info=not args.no_debug_info,
    )


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""LLVM-style naming migration for render/Core (conservative, word-boundary safe)."""
import re
from pathlib import Path

ROOT = Path(r"D:\ProgramCode\C++\Render\caustica")
SEARCH_ROOTS = [ROOT / "caustica" / "caustica", ROOT / "application"]
SKIP = {"third_party", "External", "thirdparty"}


def files():
    for base in SEARCH_ROOTS:
        if not base.exists():
            continue
        for p in base.rglob("*"):
            if p.suffix in (".h", ".cpp") and not any(s in p.parts for s in SKIP):
                yield p


def sub_word(text, pairs):
    for old, new in pairs:
        text = re.sub(r"\b" + re.escape(old) + r"\b", new, text)
    return text


def sub_member(text, fields):
    """Rename PascalCase resource fields after -> or ."""
    for old, new in fields:
        text = re.sub(r"(->|\.)" + re.escape(old) + r"\b", r"\1" + new, text)
    return text


UNIQUE_GLOBAL = [
    ("RefreshAfterLoad", "refreshAfterLoad"),
    ("GetEstimatedTextureSize", "getEstimatedTextureSize"),
    ("CompressTextures", "compressTextures"),
    ("IsValidTriangleGeometryForBlas", "isValidTriangleGeometryForBlas"),
    ("GetMeshBlasDesc", "getMeshBlasDesc"),
    ("GetMeshVerticesWorld", "getMeshVerticesWorld"),
    ("SetMeshVerticesWorld", "setMeshVerticesWorld"),
    ("GetMeshVertices", "getMeshVertices"),
    ("SetMeshVertices", "setMeshVertices"),
    ("GetUseReverseProjection", "getUseReverseProjection"),
    ("GetFramebufferInfo", "getFramebufferInfo"),
    ("GetFramebuffer", "getFramebuffer"),
    ("GetSupportedViewTypes", "getSupportedViewTypes"),
    ("SetupInputBuffers", "setupInputBuffers"),
    ("SetPushConstants", "setPushConstants"),
    ("RenderCompositeView", "renderCompositeView"),
    ("SetupMaterial", "setupMaterial"),
    ("SetupView", "setupView"),
    ("RenderView", "renderView"),
    ("CreateDescriptorHandle", "createDescriptorHandle"),
    ("GetDescriptorTableManager", "getDescriptorTableManager"),
    ("GetAllocatedCount", "getAllocatedCount"),
    ("MakeHandle", "makeHandle"),
    ("CanCompileShaders", "canCompileShaders"),
    ("IsLoadOnlyMode", "isLoadOnlyMode"),
    ("ReleaseVariant", "releaseVariant"),
    ("CreateVariant", "createVariant"),
    ("IsUpdateRequired", "isUpdateRequired"),
    ("GetNumMipLevels", "getNumMipLevels"),
    ("GBufferFramebuffer", "gBufferFramebuffer"),
    ("GBufferEmissive", "gBufferEmissive"),
    ("GBufferSpecular", "gBufferSpecular"),
    ("GBufferNormals", "gBufferNormals"),
    ("GBufferDiffuse", "gBufferDiffuse"),
    ("MotionVectors", "motionVectors"),
    ("ShadingRateSurface", "shadingRateSurface"),
    ("DepthTarget", "depthTarget"),
    ("m_DescriptorIndexMap", "m_descriptorIndexMap"),
    ("m_AllocatedDescriptors", "m_allocatedDescriptors"),
    ("m_DescriptorTable", "m_descriptorTable"),
    ("m_Descriptors", "m_descriptors"),
    ("m_SearchStart", "m_searchStart"),
    ("m_FramebufferCache", "m_framebufferCache"),
    ("m_DeferredMutex", "m_deferredMutex"),
    ("m_DeferredFrees", "m_deferredFrees"),
    ("m_GenerationCount", "m_generationCount"),
    ("m_Generations", "m_generations"),
    ("m_FreeCount", "m_freeCount"),
    ("m_FreeList", "m_freeList"),
    ("m_Manager", "m_manager"),
    ("m_Value", "m_value"),
    ("m_Mutex", "m_mutex"),
    ("m_Device", "m_device"),
    ("m_Size", "m_size"),
    ("m_SampleCount", "m_sampleCount"),
    ("m_UseReverseProjection", "m_useReverseProjection"),
    ("m_BackbufferCount", "m_backbufferCount"),
]

# Bindless + descriptor API (shared interface)
DESCRIPTOR_API = [
    ("GetDescriptorTable", "getDescriptorTable"),
    ("ReleaseDescriptor", "releaseDescriptor"),
    ("CreateDescriptor", "createDescriptor"),
    ("GetDescriptor", "getDescriptor"),
]

# BindlessHandle helpers
BINDLESS_HANDLE = [
    ("GetGeneration", "getGeneration"),
    ("GetCapacity", "getCapacity"),
    ("GetIndex", "getIndex"),
    ("GetRaw", "getRaw"),
    ("IsValid", "isValid"),
]

# ComputePipelineRegistry / ComputeShaderVariant
COMPUTE_REGISTRY = [
    ("SetVerbose", "setVerbose"),
    ("IsVerbose", "isVerbose"),
    ("GetCompileError", "getCompileError"),
    ("NeedsUpdate", "needsUpdate"),
    ("GetDebugName", "getDebugName"),
    ("GetPipeline", "getPipeline"),
]

RENDER_TARGET_FIELDS = [
    ("ProcessedOutputFramebuffer", "processedOutputFramebuffer"),
    ("DenoiserOutSpecRadianceHitDist", "denoiserOutSpecRadianceHitDist"),
    ("DenoiserOutDiffRadianceHitDist", "denoiserOutDiffRadianceHitDist"),
    ("DenoiserDisocclusionThresholdMix", "denoiserDisocclusionThresholdMix"),
    ("CombinedHistoryClampRelax", "combinedHistoryClampRelax"),
    ("SecondarySurfacePositionNormal", "secondarySurfacePositionNormal"),
    ("SecondarySurfaceRadiance", "secondarySurfaceRadiance"),
    ("DenoiserAvgLayerRadianceHalfRes", "denoiserAvgLayerRadianceHalfRes"),
    ("DenoiserSpecRadianceHitDist", "denoiserSpecRadianceHitDist"),
    ("DenoiserDiffRadianceHitDist", "denoiserDiffRadianceHitDist"),
    ("DenoiserNormalRoughness", "denoiserNormalRoughness"),
    ("DenoiserMotionVectors", "denoiserMotionVectors"),
    ("DenoiserViewspaceZ", "denoiserViewspaceZ"),
    ("DenoiserOutValidation", "denoiserOutValidation"),
    ("ScreenMotionVectors", "screenMotionVectors"),
    ("RRNormalsAndRoughness", "rrNormalsAndRoughness"),
    ("RRSpecMotionVectors", "rrSpecMotionVectors"),
    ("RRTransparencyLayer", "rrTransparencyLayer"),
    ("AccumulatedRadiance", "accumulatedRadiance"),
    ("ProcessedOutputColor", "processedOutputColor"),
    ("TemporalFeedback1", "temporalFeedback1"),
    ("TemporalFeedback2", "temporalFeedback2"),
    ("StablePlanesHeader", "stablePlanesHeader"),
    ("StablePlanesBuffer", "stablePlanesBuffer"),
    ("SurfaceDataBuffer", "surfaceDataBuffer"),
    ("StableRadiance", "stableRadiance"),
    ("OutputFramebuffer", "outputFramebuffer"),
    ("LdrColorScratch", "ldrColorScratch"),
    ("PreUIColor", "preUIColor"),
    ("RRDiffuseAlbedo", "rrDiffuseAlbedo"),
    ("RRSpecAlbedo", "rrSpecAlbedo"),
    ("RoughnessMetal", "roughnessMetal"),
    ("SpecularHitT", "specularHitT"),
    ("ScratchFloat1", "scratchFloat1"),
    ("LocalCubemap", "localCubemap"),
    ("SSRBlurMipChain", "ssrBlurMipChain"),
    ("MaterialInfo", "materialInfo"),
    ("SpecNormal", "specNormal"),
    ("BaseColor", "baseColor"),
    ("DisplaySize", "displaySize"),
    ("RenderSize", "renderSize"),
    ("OutputColor", "outputColor"),
    ("LdrColor", "ldrColor"),
    ("Throughput", "throughput"),
    ("SSRResult", "ssrResult"),
    ("RenderTargets", "renderTargets"),  # FramebufferFactory field only safe via member access
]

SCENE_GPU_FILES = {"SceneGpuUpdater.h", "SceneGpuUpdater.cpp", "SceneGeometryUpdate.cpp"}

COMPUTE_PASS_FILES = {
    "ComputePass.h", "ComputePass.cpp",
    "RtxdiPass.h", "RtxdiPass.cpp",
    "OpacityMicromapBuilder.h", "OpacityMicromapBuilder.cpp",
    "DenoisingGuidesPass.h", "DenoisingGuidesPass.cpp",
    "ZoomTool.h", "ZoomTool.cpp",
    "LightSamplingCache.h", "LightSamplingCache.cpp",
    "MaterialGpuCache.h", "MaterialGpuCache.cpp",
}

CLASS_METHOD_FILES = {
    "GBuffer.h", "GBuffer.cpp", "RenderTargets.h", "RenderTargets.cpp",
    "WorldRendererFramePasses.cpp",
}

COMPUTE_REGISTRY_FILES = {
    "ComputePipelineRegistry.h", "ComputePipelineRegistry.cpp",
    "WorldRendererFramePasses.cpp",
}


def migrate(path: Path) -> bool:
    text = path.read_text(encoding="utf-8")
    n = sub_word(text, UNIQUE_GLOBAL)
    n = sub_word(n, DESCRIPTOR_API)
    n = sub_word(n, BINDLESS_HANDLE)
    n = sub_word(n, COMPUTE_REGISTRY)
    n = sub_member(n, RENDER_TARGET_FIELDS)

    # depth field on render targets / gbuffer (member access only)
    n = sub_member(n, [("Depth", "depth"), ("Heap", "heap")])

    if path.name in SCENE_GPU_FILES:
        n = sub_word(n, [("Refresh", "refresh")])

    if path.name in COMPUTE_PASS_FILES:
        n = sub_word(n, [("Execute", "execute"), ("Init", "init")])

    if path.name in CLASS_METHOD_FILES:
        n = re.sub(r"\bRenderTargets::Init\b", "RenderTargets::init", n)
        n = re.sub(r"\bRenderTargets::Clear\b", "RenderTargets::clear", n)
        n = re.sub(r"\bGBufferRenderTargets::Init\b", "GBufferRenderTargets::init", n)
        n = re.sub(r"\bGBufferRenderTargets::Clear\b", "GBufferRenderTargets::clear", n)
        n = re.sub(r"m_renderTargets->Init\b", "m_renderTargets->init", n)
        n = re.sub(r"m_renderTargets->Clear\b", "m_renderTargets->clear", n)
        n = re.sub(r"renderTargets->Init\b", "renderTargets->init", n)
        n = re.sub(r"renderTargets->Clear\b", "renderTargets->clear", n)

    if path.name in COMPUTE_REGISTRY_FILES:
        n = re.sub(r"\bComputePipelineRegistry::Update\b", "ComputePipelineRegistry::update", n)
        n = re.sub(r"computePipelines\(\)->Update\b", "computePipelines()->update", n)
        n = re.sub(r"void Update\(bool forceReload", "void update(bool forceReload", n)

    # GBuffer / RenderTargets header field declarations
    if path.name in {"GBuffer.h", "RenderTargets.h"}:
        for old, new in RENDER_TARGET_FIELDS + [("Depth", "depth"), ("Heap", "heap")]:
            n = re.sub(r"\b" + re.escape(old) + r"\s*;", new + ";", n)
            n = re.sub(r"\b" + re.escape(old) + r"\[", new + "[", n)
            n = re.sub(r"\b" + re.escape(old) + r"\s*=", new + " =", n)

    if path.name == "GBuffer.h":
        n = sub_word(n, [("GetSampleCount", "getSampleCount"), ("GetSize", "getSize")])

    if n != text:
        path.write_text(n, encoding="utf-8")
        return True
    return False


def main():
    changed = [str(p.relative_to(ROOT)) for p in files() if migrate(p)]
    print(f"Updated {len(changed)} files")
    for c in sorted(changed):
        print(c)


if __name__ == "__main__":
    main()

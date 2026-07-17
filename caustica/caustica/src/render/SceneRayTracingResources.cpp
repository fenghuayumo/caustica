#include <render/SceneRayTracingResources.h>

#include <render/PathTracerScenePasses.h>
#include <render/SceneLightingPasses.h>
#include <render/worldRenderer/WorldRenderer.h>

#include <backend/GpuDevice.h>
#include <render/core/BindingCache.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <render/core/AccelStructManager.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <scene/Scene.h>

#include <shaders/PathTracer/Lighting/LightingTypes.hlsli>

namespace caustica::render
{

void SceneRayTracingResources::wireSession(const ScenePassWireParams& params)
{
    m_gpuDevice = &params.gpuDevice;
    m_accelStructs = &params.accelStructs;
    m_worldRenderer = &params.worldRenderer;
    m_settings = &params.settings;
    m_invalidation = &params.invalidation;
    m_lightingPasses = &params.lighting;
    m_bindingCache = &params.bindingCache;
}

void SceneRayTracingResources::setAdditionalAccelStructBuilder(AdditionalAccelStructBuilder builder)
{
    m_additionalAccelStructBuilder = std::move(builder);
}

void SceneRayTracingResources::fillPTPipelineGlobalMacros(std::vector<caustica::ShaderMacro>& macros)
{
    macros.clear();

    auto* device = m_gpuDevice->getDevice();
    const bool canUseNvapiHitObject =
        m_settings->NVAPIHitObjectExtension &&
        device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12 &&
        device->queryFeatureSupport(nvrhi::Feature::HlslExtensionUAV);
    const bool canUseDxHitObject =
        m_settings->DXHitObjectExtension &&
        device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12;

    assert(!canUseNvapiHitObject || !canUseDxHitObject);

    macros.push_back({ "ENABLE_DEBUG_SURFACE_VIZ",  (m_settings->DebugView != DebugViewType::Disabled)?("1"):("0") });
    macros.push_back({ "ENABLE_DEBUG_LINES_VIZ",    (m_settings->ShowDebugLines)?("1"):("0") });

    macros.push_back({ "USE_NVAPI_HIT_OBJECT_EXTENSION", canUseNvapiHitObject ? "1" : "0" });
    macros.push_back({ "USE_NVAPI_REORDER_THREADS", (canUseNvapiHitObject && m_settings->NVAPIReorderThreads) ? "1" : "0" });

    macros.push_back({ "USE_DX_HIT_OBJECT_EXTENSION", canUseDxHitObject ? "1" : "0" });
    macros.push_back({ "USE_DX_MAYBE_REORDER_THREADS", (canUseDxHitObject && m_settings->DXMaybeReorderThreads) ? "1" : "0" });

    macros.push_back({ "PT_ENABLE_RUSSIAN_ROULETTE", (m_settings->EnableRussianRoulette) ? ("1") : ("0") });
    macros.push_back({ "PT_NEE_ENABLED", (m_settings->UseNEE)?("1"):("0") });
    macros.push_back({ "PT_USE_RESTIR_DI", (m_settings->actualUseReSTIRDI()) ? ("1") : ("0") });
    macros.push_back({ "PT_USE_RESTIR_GI", (m_settings->actualUseReSTIRGI()) ? ("1") : ("0") });
    macros.push_back({ "PT_USE_RESTIR_PT", (m_settings->actualUseReSTIRPT()) ? ("1") : ("0") });

    const std::shared_ptr<OpacityMicromapBuilder>& opacityMaps = m_lightingPasses->opacityMaps();
    const bool useOpacityMicromaps = opacityMaps != nullptr && opacityMaps->shouldUseRayTracingOpacityMicromaps();
    macros.push_back({ "CAUSTICA_ENABLE_OPACITY_MICROMAPS", useOpacityMicromaps ? "1" : "0" });

    macros.push_back({ "CAUSTICA_USE_APPROXIMATE_MIS", (m_settings->actualUseApproximateMIS()) ? ("1") : ("0") });

    macros.push_back({ "CAUSTICA_NEE_FULL_SAMPLE_COUNT", std::to_string(m_settings->NEEFullSamples) });
    uint localCandidateSamples = ComputeCandidateSampleLocalCount(m_settings->ActualNEEAT_LocalToGlobalSampleRatio(), m_settings->NEECandidateSamples);
    uint globalCandidateSamples = ComputeCandidateSampleGlobalCount(m_settings->ActualNEEAT_LocalToGlobalSampleRatio(), m_settings->NEECandidateSamples);
    macros.push_back({ "CAUSTICA_NEE_LOCAL_CANDIDATE_SAMPLE_COUNT", std::to_string(localCandidateSamples) });
    macros.push_back({ "CAUSTICA_NEE_GLOBAL_CANDIDATE_SAMPLE_COUNT", std::to_string(globalCandidateSamples) });
    macros.push_back({ "CAUSTICA_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT", std::to_string(m_settings->NEECandidateSamples) });

    macros.push_back({ "CAUSTICA_DISABLE_SER_TERMINATION_HINT", (m_settings->DbgDisableSERTerminationHint)?("1"):("0") });
    macros.push_back({ "CAUSTICA_DISCARD_NON_NEE_LIGHTING", (m_settings->DbgDiscardNonNEELighting) ? ("1") : ("0") });
    macros.push_back({ "CAUSTICA_DISCARD_NEE_LIGHTING", (m_settings->DbgDiscardNEELighting) ? ("1") : ("0") });

    macros.push_back({ "CAUSTICA_FIREFLY_FILTER", (m_settings->actualFireflyFilterEnabled()) ? ("1") : ("0") });
    macros.push_back({ "CAUSTICA_ACTIVE_STABLE_PLANE_COUNT", std::to_string(m_settings->StablePlanesActiveCount) });
    macros.push_back({ "CAUSTICA_NESTED_DIELECTRICS_QUALITY", std::to_string(m_settings->NestedDielectricsQuality) });
    macros.push_back({ "CAUSTICA_LP_TYPES_USE_16BIT_PRECISION", (m_settings->UseFp16Types) ? ("1") : ("0") });
    macros.push_back({ "CAUSTICA_ENABLE_LOW_DISCREPANCY_SAMPLER_FOR_BSDF", (m_settings->EnableLDSamplerForBSDF) ? ("1") : ("0") });

    m_lightingPasses->applyShaderMacros(macros);
}

bool SceneRayTracingResources::createPTPipeline()
{
    return m_worldRenderer->createPTPipeline();
}

void SceneRayTracingResources::createRTPipelines()
{
    auto compiler = pathTracingShaderCompiler();
    using SM = caustica::ShaderMacro;
    pipelineReference() = compiler->createVariant("PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_REFERENCE") }, "REF");
    ensureStablePlanePipelines();
    if (m_settings->PostProcessTestPassHDR)
        pipelineTestRaygenPPHDR() = compiler->createVariant("TestRaygenPP.hlsl", { SM("PP_TEST_HDR", "1") }, "TESTRG", true);
    if (m_settings->PostProcessEdgeDetection)
        pipelineEdgeDetection() = compiler->createVariant("TestRaygenPP.hlsl", { SM("PP_EDGE_DETECTION", "1") }, "EDGY", true);
}

void SceneRayTracingResources::ensureStablePlanePipelines()
{
    auto compiler = pathTracingShaderCompiler();
    if (!compiler)
        return;

    using SM = caustica::ShaderMacro;
    if (!pipelineBuildStablePlanes())
    {
        pipelineBuildStablePlanes() = compiler->createVariant(
            "PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_BUILD_STABLE_PLANES") }, "BUILD");
    }
    if (!pipelineFillStablePlanes())
    {
        pipelineFillStablePlanes() = compiler->createVariant(
            "PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_FILL_STABLE_PLANES") }, "FILL");
    }
}

void SceneRayTracingResources::createBlases(
    nvrhi::ICommandList* commandList,
    const caustica::scene::SceneRenderData& renderData)
{
    caustica::AccelStructBuildSettings settings = { .excludeTransmissive = m_settings->AS.ExcludeTransmissive };
    m_accelStructs->bindMaterialGpuCache(m_lightingPasses->materials().get());
    m_accelStructs->createBlases(commandList, renderData.meshSnapshots, settings);
}

void SceneRayTracingResources::uploadSubInstanceData(nvrhi::ICommandList* commandList)
{
    m_accelStructs->uploadSubInstanceData(commandList);
}

void SceneRayTracingResources::createTlas(
    nvrhi::ICommandList* commandList,
    const caustica::scene::SceneRenderData& renderData)
{
    m_accelStructs->createTlas(commandList, renderData);
}

void SceneRayTracingResources::createAccelStructs(
    nvrhi::ICommandList* commandList,
    caustica::Scene& scene,
    const caustica::scene::SceneRenderData* renderData)
{
    const caustica::scene::SceneRenderData& data =
        renderData ? *renderData : scene.getRenderData();
    m_lightingPasses->createOpacityMicromaps(data);
    createBlases(commandList, data);
    createTlas(commandList, data);
    if (m_additionalAccelStructBuilder)
        m_additionalAccelStructBuilder(commandList);
}

void SceneRayTracingResources::recreateAccelStructs(
    nvrhi::ICommandList* commandList,
    caustica::Scene& scene,
    const caustica::scene::SceneRenderData* renderData)
{
    if (!m_invalidation->AccelerationStructRebuildRequested)
        return;

    m_invalidation->AccelerationStructRebuildRequested = false;
    m_settings->ResetAccumulation = true;

    if (!m_gpuDevice->getDevice()->waitForIdle())
        return;

    m_worldRenderer->invalidateBindingSet();
    m_accelStructs->releaseGpuResources();
    const caustica::scene::SceneRenderData& data =
        renderData ? *renderData : scene.getRenderData();
    m_accelStructs->clearMeshAccelStructs(data.meshSnapshots);
    m_gpuDevice->getDevice()->runGarbageCollection();

    commandList->open();
    createAccelStructs(commandList, scene, renderData);
    commandList->close();
    m_gpuDevice->getDevice()->executeCommandList(commandList);
    m_gpuDevice->getDevice()->waitForIdle();
}

void SceneRayTracingResources::requestMeshAccelRebuild(const std::shared_ptr<caustica::MeshInfo>& mesh, bool resetAccumulation)
{
    if (!mesh)
        return;

    if (resetAccumulation)
        m_settings->ResetAccumulation = true;

    if (!m_accelStructs->hasTopLevelAS())
    {
        m_invalidation->AccelerationStructRebuildRequested = true;
        return;
    }

    m_accelStructs->requestMeshRebuild(mesh->renderResourceId);
}

void SceneRayTracingResources::requestAccelerationStructureRebuild()
{
    m_invalidation->AccelerationStructRebuildRequested = true;
    m_settings->ResetAccumulation = true;
    // Binding set is invalidated again inside recreateAccelStructs once GPU is idle.
}

void SceneRayTracingResources::requestFullRebuild()
{
    m_invalidation->AccelerationStructRebuildRequested = true;
    m_invalidation->ShaderReloadRequested = true;
    m_invalidation->ShaderAndACRefreshDelayedRequest = 0.0f;
    m_settings->ResetAccumulation = true;
    m_worldRenderer->invalidateBindingSet();
    if (m_bindingCache)
        m_bindingCache->clear();
}

void SceneRayTracingResources::invalidateBindingSet()
{
    m_worldRenderer->invalidateBindingSet();
}

void SceneRayTracingResources::recreateBindingSet(
    const caustica::scene::SceneRenderData* renderData)
{
    m_worldRenderer->recreateBindingSet(renderData);
}

void SceneRayTracingResources::sampleRenderCode(nvrhi::IFramebuffer* framebuffer,
    nvrhi::CommandListHandle commandList,
    const SampleConstants& constants)
{
    (void)framebuffer;
    (void)commandList;
    (void)constants;
}

bool SceneRayTracingResources::consumeShaderReloadRequest()
{
    if (!m_invalidation->ShaderReloadRequested)
        return false;
    m_invalidation->ShaderReloadRequested = false;
    return true;
}

bool& SceneRayTracingResources::accelerationStructRebuildRequested()
{
    return m_invalidation->AccelerationStructRebuildRequested;
}

std::shared_ptr<PathTracingShaderCompiler> SceneRayTracingResources::pathTracingShaderCompiler() const
{
    return m_worldRenderer->getPathTracingShaderCompiler();
}

std::shared_ptr<PTPipelineVariant>& SceneRayTracingResources::pipelineReference()
{
    return m_worldRenderer->ptPipelineReference();
}

std::shared_ptr<PTPipelineVariant>& SceneRayTracingResources::pipelineBuildStablePlanes()
{
    return m_worldRenderer->ptPipelineBuildStablePlanes();
}

std::shared_ptr<PTPipelineVariant>& SceneRayTracingResources::pipelineFillStablePlanes()
{
    return m_worldRenderer->ptPipelineFillStablePlanes();
}

std::shared_ptr<PTPipelineVariant>& SceneRayTracingResources::pipelineTestRaygenPPHDR()
{
    return m_worldRenderer->ptPipelineTestRaygenPPHDR();
}

std::shared_ptr<PTPipelineVariant>& SceneRayTracingResources::pipelineEdgeDetection()
{
    return m_worldRenderer->ptPipelineEdgeDetection();
}

} // namespace caustica::render

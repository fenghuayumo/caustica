#include <render/SceneRayTracingResources.h>

#include <render/SceneLightingPasses.h>
#include <render/WorldRenderer/PathTracingWorldRenderer.h>

#include <backend/GpuDevice.h>
#include <render/Core/BindingCache.h>
#include <render/Core/PTPipelineBaker.h>
#include <render/Core/RenderCore.h>
#include <scene/SceneManager.h>
#include <scene/Scene.h>

#include <shaders/PathTracer/Lighting/LightingTypes.hlsli>

namespace caustica::editor
{

void SceneRayTracingResources::attach(caustica::GpuDevice& gpuDevice,
    SceneManager& sceneManager,
    caustica::RenderCore& renderCore,
    caustica::render::PathTracingWorldRenderer& worldRenderer,
    PathTracerSettings& settings,
    caustica::render::RenderInvalidationState& invalidation,
    SceneLightingPasses& lightingPasses,
    caustica::BindingCache& bindingCache)
{
    m_gpuDevice = &gpuDevice;
    m_sceneManager = &sceneManager;
    m_renderCore = &renderCore;
    m_worldRenderer = &worldRenderer;
    m_settings = &settings;
    m_invalidation = &invalidation;
    m_lightingPasses = &lightingPasses;
    m_bindingCache = &bindingCache;
}

void SceneRayTracingResources::setAdditionalAccelStructBuilder(AdditionalAccelStructBuilder builder)
{
    m_additionalAccelStructBuilder = std::move(builder);
}

void SceneRayTracingResources::fillPTPipelineGlobalMacros(std::vector<caustica::ShaderMacro>& macros)
{
    macros.clear();

    auto* device = m_gpuDevice->GetDevice();
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
    macros.push_back({ "PT_USE_RESTIR_DI", (m_settings->ActualUseReSTIRDI()) ? ("1") : ("0") });
    macros.push_back({ "PT_USE_RESTIR_GI", (m_settings->ActualUseReSTIRGI()) ? ("1") : ("0") });
    macros.push_back({ "PT_USE_RESTIR_PT", (m_settings->ActualUseReSTIRPT()) ? ("1") : ("0") });

    macros.push_back({ "CAUSTICA_USE_APPROXIMATE_MIS", (m_settings->ActualUseApproximateMIS()) ? ("1") : ("0") });

    macros.push_back({ "CAUSTICA_NEE_FULL_SAMPLE_COUNT", std::to_string(m_settings->NEEFullSamples) });
    uint localCandidateSamples = ComputeCandidateSampleLocalCount(m_settings->ActualNEEAT_LocalToGlobalSampleRatio(), m_settings->NEECandidateSamples);
    uint globalCandidateSamples = ComputeCandidateSampleGlobalCount(m_settings->ActualNEEAT_LocalToGlobalSampleRatio(), m_settings->NEECandidateSamples);
    macros.push_back({ "CAUSTICA_NEE_LOCAL_CANDIDATE_SAMPLE_COUNT", std::to_string(localCandidateSamples) });
    macros.push_back({ "CAUSTICA_NEE_GLOBAL_CANDIDATE_SAMPLE_COUNT", std::to_string(globalCandidateSamples) });
    macros.push_back({ "CAUSTICA_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT", std::to_string(m_settings->NEECandidateSamples) });

    macros.push_back({ "CAUSTICA_DISABLE_SER_TERMINATION_HINT", (m_settings->DbgDisableSERTerminationHint)?("1"):("0") });
    macros.push_back({ "CAUSTICA_DISCARD_NON_NEE_LIGHTING", (m_settings->DbgDiscardNonNEELighting) ? ("1") : ("0") });
    macros.push_back({ "CAUSTICA_DISCARD_NEE_LIGHTING", (m_settings->DbgDiscardNEELighting) ? ("1") : ("0") });

    macros.push_back({ "CAUSTICA_FIREFLY_FILTER", (m_settings->ActualFireflyFilterEnabled()) ? ("1") : ("0") });
    macros.push_back({ "CAUSTICA_ACTIVE_STABLE_PLANE_COUNT", std::to_string(m_settings->StablePlanesActiveCount) });
    macros.push_back({ "CAUSTICA_NESTED_DIELECTRICS_QUALITY", std::to_string(m_settings->NestedDielectricsQuality) });
    macros.push_back({ "CAUSTICA_LP_TYPES_USE_16BIT_PRECISION", (m_settings->UseFp16Types) ? ("1") : ("0") });
    macros.push_back({ "CAUSTICA_ENABLE_LOW_DISCREPANCY_SAMPLER_FOR_BSDF", (m_settings->EnableLDSamplerForBSDF) ? ("1") : ("0") });

    m_lightingPasses->applyBakerShaderMacros(macros);
}

bool SceneRayTracingResources::createPTPipeline()
{
    return m_worldRenderer->createPTPipeline();
}

void SceneRayTracingResources::createRTPipelines()
{
    auto pipelineBaker = getPipelineBaker();
    using SM = caustica::ShaderMacro;
    pipelineReference()         = pipelineBaker->CreateVariant("PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_REFERENCE") }, "REF");
    pipelineBuildStablePlanes() = pipelineBaker->CreateVariant("PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_BUILD_STABLE_PLANES") }, "BUILD");
    pipelineFillStablePlanes()  = pipelineBaker->CreateVariant("PathTracerSample.hlsl", { SM("PATH_TRACER_MODE", "PATH_TRACER_MODE_FILL_STABLE_PLANES") }, "FILL");
    pipelineTestRaygenPPHDR()   = pipelineBaker->CreateVariant("TestRaygenPP.hlsl", { SM("PP_TEST_HDR", "1") }, "TESTRG", true);
    pipelineEdgeDetection()     = pipelineBaker->CreateVariant("TestRaygenPP.hlsl", { SM("PP_EDGE_DETECTION", "1") }, "EDGY", true);
}

void SceneRayTracingResources::createBlases(nvrhi::ICommandList* commandList)
{
    caustica::AccelStructBuildSettings settings = { .excludeTransmissive = m_settings->AS.ExcludeTransmissive };
    m_renderCore->accelStructs().createBlases(commandList, *m_sceneManager->getScene(), settings);
}

void SceneRayTracingResources::uploadSubInstanceData(nvrhi::ICommandList* commandList)
{
    m_renderCore->accelStructs().uploadSubInstanceData(commandList);
}

void SceneRayTracingResources::createTlas(nvrhi::ICommandList* commandList)
{
    m_renderCore->accelStructs().createTlas(commandList, *m_sceneManager->getScene());
}

void SceneRayTracingResources::createAccelStructs(nvrhi::ICommandList* commandList)
{
    m_lightingPasses->createOpacityMicromaps(*m_sceneManager->getScene());
    createBlases(commandList);
    createTlas(commandList);
    if (m_additionalAccelStructBuilder)
        m_additionalAccelStructBuilder(commandList);
}

void SceneRayTracingResources::recreateAccelStructs(nvrhi::ICommandList* commandList)
{
    if (!m_invalidation->AccelerationStructRebuildRequested)
        return;

    m_invalidation->AccelerationStructRebuildRequested = false;
    m_settings->ResetAccumulation = true;

    m_gpuDevice->GetDevice()->waitForIdle();

    m_worldRenderer->invalidateBindingSet();
    m_renderCore->accelStructs().releaseGpuResources();
    m_renderCore->accelStructs().clearMeshAccelStructs(*m_sceneManager->getScene());
    m_gpuDevice->GetDevice()->runGarbageCollection();

    commandList->open();
    createAccelStructs(commandList);
    commandList->close();
    m_gpuDevice->GetDevice()->executeCommandList(commandList);
    m_gpuDevice->GetDevice()->waitForIdle();
}

void SceneRayTracingResources::requestMeshAccelRebuild(const std::shared_ptr<caustica::MeshInfo>& mesh)
{
    if (!mesh)
        return;

    m_settings->ResetAccumulation = true;

    if (!m_renderCore->accelStructs().hasTopLevelAS())
    {
        m_invalidation->AccelerationStructRebuildRequested = true;
        return;
    }

    m_renderCore->accelStructs().requestMeshRebuild(mesh);
}

void SceneRayTracingResources::requestFullRebuild()
{
    m_invalidation->AccelerationStructRebuildRequested = true;
    m_invalidation->ShaderReloadRequested = true;
    m_invalidation->ShaderAndACRefreshDelayedRequest = 0.0f;
    m_settings->ResetAccumulation = true;
    m_worldRenderer->invalidateBindingSet();
    if (m_bindingCache)
        m_bindingCache->Clear();
}

void SceneRayTracingResources::invalidateBindingSet()
{
    m_worldRenderer->invalidateBindingSet();
}

void SceneRayTracingResources::recreateBindingSet()
{
    m_worldRenderer->recreateBindingSet();
}

void SceneRayTracingResources::sampleRenderCode(nvrhi::IFramebuffer* framebuffer,
    nvrhi::CommandListHandle commandList,
    const SampleConstants& constants)
{
    if (m_settings->ActualUseRTXDIPasses())
        m_worldRenderer->getRtxdiPass()->BeginFrame(commandList, *m_worldRenderer->getRenderTargets(), m_worldRenderer->getBindingLayout(), m_worldRenderer->getBindingSet());

    m_worldRenderer->pathTrace(framebuffer, constants);
    m_worldRenderer->denoise(framebuffer);
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

std::shared_ptr<PTPipelineBaker> SceneRayTracingResources::getPipelineBaker() const
{
    return m_worldRenderer->getPTPipelineBaker();
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

} // namespace caustica::editor

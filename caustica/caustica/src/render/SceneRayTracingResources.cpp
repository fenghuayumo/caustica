#include <render/SceneRayTracingResources.h>

#include <render/PathTracerScenePasses.h>
#include <render/SceneLightingPasses.h>
#include <render/WorldRenderer.h>

#include <backend/GpuDevice.h>
#include <core/log.h>
#include <render/core/BindingCache.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <render/core/RtPipelineCache.h>
#include <render/core/AccelStructManager.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <scene/Scene.h>

#include <cassert>
#include <vector>

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

PtFeaturePresetId SceneRayTracingResources::resolveFeaturePreset() const
{
    PtFeaturePresetResolveInput input;
    input.settings = m_settings;
    const std::shared_ptr<OpacityMicromapBuilder>& opacityMaps = m_lightingPasses->opacityMaps();
    input.useOpacityMicromaps = opacityMaps != nullptr && opacityMaps->shouldUseRayTracingOpacityMicromaps();
    if (m_lightingPasses->lightSampling())
        input.sampleBakedEnvironment = m_lightingPasses->lightSampling()->sampleBakedEnvironment();
    return resolvePtFeaturePreset(input);
}

void SceneRayTracingResources::fillPTPipelineGlobalMacros(std::vector<caustica::ShaderMacro>& macros)
{
    // Always emit a cooked preset macro list so runtime hashes match offline bins / RT PSO cache.
    fillPtFeaturePresetMacros(resolveFeaturePreset(), macros);
}

bool SceneRayTracingResources::createPTPipeline()
{
    return m_worldRenderer->createPTPipeline();
}

void SceneRayTracingResources::createRTPipelines()
{
    auto compiler = pathTracingShaderCompiler();
    assert(compiler);
    auto cache = m_worldRenderer->getRtPipelineCache();
    assert(cache);

    // UE-style startup: only the active cooked preset. Other presets stay cold until
    // first use, or explicit RtPipelineCache::precacheAll during load/cook.
    const PtFeaturePresetId active = resolveFeaturePreset();
    cache->ensurePresetVariants(active);

    using SM = caustica::ShaderMacro;
    // Optional editor-only raygen variants stay on the Default macro set.
    std::vector<caustica::ShaderMacro> defaultMacros;
    fillPtFeaturePresetMacros(PtFeaturePresetId::Default, defaultMacros);
    if (m_settings->PostProcessTestPassHDR && !pipelineTestRaygenPPHDR())
    {
        pipelineTestRaygenPPHDR() = compiler->createVariant(
            "TestRaygenPP.hlsl", { SM("PP_TEST_HDR", "1") }, "TESTRG", true, defaultMacros);
    }
    if (m_settings->PostProcessEdgeDetection && !pipelineEdgeDetection())
    {
        pipelineEdgeDetection() = compiler->createVariant(
            "TestRaygenPP.hlsl", { SM("PP_EDGE_DETECTION", "1") }, "EDGY", true, defaultMacros);
    }

    // Bind variant pointers only. CreateStateObject must wait until
    // PathTracingShaderCompiler::update() has built the hit-group export set
    // (materials loaded + scene sub-instances). Calling ensureReady here
    // CreateStateObjects with an empty hit-group map and crashes / freezes PSOs.
    if (!bindFeaturePreset(active))
    {
        caustica::error(
            "RtPipelineCache: failed to bind preset '%s' after createRTPipelines",
            ptFeaturePresetName(active).data());
    }
}

void SceneRayTracingResources::ensureStablePlanePipelines()
{
    auto cache = m_worldRenderer->getRtPipelineCache();
    if (!cache)
        return;
    cache->ensurePresetVariants(cache->activePreset());
    bindFeaturePreset(cache->activePreset());
}

bool SceneRayTracingResources::bindFeaturePreset(PtFeaturePresetId id)
{
    auto cache = m_worldRenderer->getRtPipelineCache();
    if (!cache)
        return false;

    cache->ensurePresetVariants(id);
    const PtFeaturePresetId previous = cache->activePreset();
    if (!cache->bind(id, pipelineReference(), pipelineBuildStablePlanes(), pipelineFillStablePlanes()))
        return false;

    if (previous != id)
    {
        caustica::info("RtPipelineCache: bound feature preset '%s'", ptFeaturePresetName(id).data());
        m_settings->ResetAccumulation = true;
    }
    return true;
}

bool SceneRayTracingResources::ensureFeaturePresetReady(PtFeaturePresetId id, bool showProgress)
{
    auto cache = m_worldRenderer->getRtPipelineCache();
    if (!cache)
        return false;

    // Single CreateStateObject owner: RtPipelineCache::ensureReady / buildPreset.
    if (!cache->ensureReady(id, showProgress))
        return false;
    return bindFeaturePreset(id);
}

void SceneRayTracingResources::createBlases(
    caustica::rhi::CommandList* commandList,
    const caustica::scene::SceneRenderData& renderData)
{
    caustica::AccelStructBuildSettings settings = { .excludeTransmissive = m_settings->AS.ExcludeTransmissive };
    m_accelStructs->bindMaterialGpuCache(m_lightingPasses->materials().get());
    m_accelStructs->createBlases(commandList, renderData.meshSnapshots, settings);
}

void SceneRayTracingResources::uploadSubInstanceData(caustica::rhi::CommandList* commandList)
{
    m_accelStructs->uploadSubInstanceData(commandList);
}

void SceneRayTracingResources::createTlas(
    caustica::rhi::CommandList* commandList,
    const caustica::scene::SceneRenderData& renderData)
{
    m_accelStructs->createTlas(commandList, renderData);
}

void SceneRayTracingResources::createAccelStructs(
    caustica::rhi::CommandList* commandList,
    caustica::Scene& scene,
    const caustica::scene::SceneRenderData* renderData)
{
    (void)scene;
    assert(renderData && "createAccelStructs requires published SceneRenderData");
    const caustica::scene::SceneRenderData& data = *renderData;
    m_lightingPasses->createOpacityMicromaps(data);
    createBlases(commandList, data);
    createTlas(commandList, data);
    if (m_additionalAccelStructBuilder)
        m_additionalAccelStructBuilder(commandList);
}

void SceneRayTracingResources::recreateAccelStructs(
    caustica::rhi::CommandList* commandList,
    caustica::Scene& scene,
    const caustica::scene::SceneRenderData* renderData)
{
    if (!m_invalidation->AccelerationStructRebuildRequested)
        return;

    m_invalidation->AccelerationStructRebuildRequested = false;
    m_settings->ResetAccumulation = true;

    assert(renderData && "recreateAccelStructs requires published SceneRenderData");

    // Double-buffered rebuild: keep the previous TLAS/BLAS generation alive in
    // AccelStructManager retired lists so in-flight DispatchRays can finish while
    // this CL builds the new generation. No device-wide waitForIdle — previous
    // frame GPU work overlaps with the AS build on the graphics queue.
    m_accelStructs->clearRetiredAccelStructs();
    m_worldRenderer->invalidateBindingSet();

    commandList->open();
    createAccelStructs(commandList, scene, renderData);
    commandList->close();
    m_gpuDevice->getDevice()->executeCommandList(commandList);
    // Binding-set recreate (caller) points at the new TLAS; retired handles keep
    // the old generation valid until the next structure rebuild clears them.
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

void SceneRayTracingResources::sampleRenderCode(caustica::rhi::Framebuffer* framebuffer,
    caustica::rhi::CommandListHandle commandList,
    const FrameConstants& constants)
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

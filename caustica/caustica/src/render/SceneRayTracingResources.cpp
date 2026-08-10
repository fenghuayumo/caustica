#include <render/SceneRayTracingResources.h>

#include <render/PathTraceSceneBindings.h>
#include <render/SceneLightingPasses.h>

#include <backend/GpuDevice.h>
#include <core/log.h>
#include <render/core/BindingCache.h>
#include <render/core/PathTracerSettings.h>
#include <render/core/StreamingUploadBudget.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <render/core/RtPipelineCache.h>
#include <render/core/AccelStructManager.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <scene/Scene.h>
#include <scene/internal/RenderResourceAccess.h>

#include <cassert>
#include <algorithm>
#include <span>
#include <vector>

using caustica::scene::internal::RenderResourceAccess;

namespace caustica::render
{

void SceneRayTracingResources::initialize(
    const Dependencies& dependencies,
    SceneLightingPasses& lighting)
{
    m_gpuDevice = &dependencies.gpuDevice;
    m_accelStructs = &dependencies.accelStructs;
    m_invalidation = &dependencies.invalidation;
    m_lightingPasses = &lighting;
    m_bindingCache = &dependencies.bindingCache;
    m_sceneBindings = &dependencies.sceneBindings;
}

void SceneRayTracingResources::setAdditionalAccelStructBuilder(AdditionalAccelStructBuilder builder)
{
    m_additionalAccelStructBuilder = std::move(builder);
}

PtFeaturePresetId SceneRayTracingResources::resolveFeaturePreset(
    const PathTracerSettings& settings) const
{
    PtFeaturePresetResolveInput input;
    input.settings = &settings;
    const std::shared_ptr<OpacityMicromapBuilder>& opacityMaps = m_lightingPasses->opacityMaps();
    input.useOpacityMicromaps = opacityMaps != nullptr && opacityMaps->shouldUseRayTracingOpacityMicromaps();
    if (m_lightingPasses->lightSampling())
        input.sampleBakedEnvironment = m_lightingPasses->lightSampling()->sampleBakedEnvironment();
    return resolvePtFeaturePreset(input);
}

void SceneRayTracingResources::fillPTPipelineGlobalMacros(
    std::vector<caustica::ShaderMacro>& macros,
    const PathTracerSettings& settings)
{
    // Always emit a cooked preset macro list so runtime hashes match offline bins / RT PSO cache.
    fillPtFeaturePresetMacros(resolveFeaturePreset(settings), macros);
}

void SceneRayTracingResources::initializePipelineRuntime(
    caustica::rhi::BindingLayoutHandle bindingLayout,
    caustica::rhi::BindingLayoutHandle bindlessLayout,
    const PathTracerSettings& settings)
{
    assert(!m_shaderCompiler && !m_pipelineCache);
    m_shaderCompiler = std::make_shared<PathTracingShaderCompiler>(
        m_gpuDevice->getDevice(), m_lightingPasses->materials(), bindingLayout, bindlessLayout);
    m_pipelineCache = std::make_shared<RtPipelineCache>(m_shaderCompiler);
    createRTPipelines(settings);
}

void SceneRayTracingResources::updatePipelineRuntime(
    const caustica::scene::SceneRenderData* sceneData,
    uint32_t subInstanceCount,
    bool forceShaderReload,
    const PathTracerSettings& settings)
{
    if (!m_shaderCompiler || !m_pipelineCache)
        return;

    const PtFeaturePresetId desiredPreset = resolveFeaturePreset(settings);
    if (m_pipelineCache->activePreset() != desiredPreset)
        m_invalidation->AccumulationResetRequested = true;

    m_shaderCompiler->update(
        sceneData,
        subInstanceCount,
        [this, &settings](std::vector<caustica::ShaderMacro>& macros) {
            fillPTPipelineGlobalMacros(macros, settings);
        },
        forceShaderReload);

    if (m_shaderCompiler->hasUniqueHitGroups()
        && (m_pipelineCache->activePreset() != desiredPreset
            || !m_pipelineCache->isReady(desiredPreset)
            || !m_pipelineReference))
    {
        ensureFeaturePresetReady(desiredPreset, /*showProgress=*/false);
    }
}

RtPipelineWarmupStatus SceneRayTracingResources::pipelineWarmupStatus() const
{
    return m_pipelineCache ? m_pipelineCache->status() : RtPipelineWarmupStatus{};
}

RtPipelineCacheStats SceneRayTracingResources::pipelineCacheStats() const
{
    return m_pipelineCache ? m_pipelineCache->stats() : RtPipelineCacheStats{};
}

void SceneRayTracingResources::createRTPipelines(const PathTracerSettings& settings)
{
    assert(m_shaderCompiler);
    assert(m_pipelineCache);

    // UE-style startup: only the active cooked preset. Other presets stay cold until
    // first use, or explicit RtPipelineCache::precacheAll during load/cook.
    const PtFeaturePresetId active = resolveFeaturePreset(settings);
    m_pipelineCache->ensurePresetVariants(active);

    using SM = caustica::ShaderMacro;
    // Optional editor-only raygen variants stay on the Default macro set.
    std::vector<caustica::ShaderMacro> defaultMacros;
    fillPtFeaturePresetMacros(PtFeaturePresetId::Default, defaultMacros);
    if (settings.PostProcessEdgeDetection && !m_pipelineEdgeDetection)
    {
        m_pipelineEdgeDetection = m_shaderCompiler->createVariant(
            "TestRaygenPP.hlsl", { SM("PP_EDGE_DETECTION", "1") }, "EDGY", true, defaultMacros);
    }

    // Publish the stable variant objects before the frame graph is registered.
    // This does not create a state object: buildPreset() still waits until
    // PathTracingShaderCompiler::update() has produced the hit-group exports.
    // The PSO and SBT are filled into these same objects later, matching RTXPT's
    // persistent pipeline-handle model.
    if (!bindFeaturePreset(active))
    {
        caustica::error(
            "RtPipelineCache: failed to publish preset '%s' after createRTPipelines",
            ptFeaturePresetName(active).data());
    }
}

void SceneRayTracingResources::ensureStablePlanePipelines()
{
    if (!m_pipelineCache)
        return;
    m_pipelineCache->ensurePresetVariants(m_pipelineCache->activePreset());
    if (m_pipelineCache->isReady(m_pipelineCache->activePreset()))
        bindFeaturePreset(m_pipelineCache->activePreset());
}

bool SceneRayTracingResources::bindFeaturePreset(PtFeaturePresetId id)
{
    if (!m_pipelineCache)
        return false;

    m_pipelineCache->ensurePresetVariants(id);
    const PtFeaturePresetId previous = m_pipelineCache->activePreset();
    if (!m_pipelineCache->bind(
            id, m_pipelineReference, m_pipelineBuildStablePlanes, m_pipelineFillStablePlanes))
        return false;

    if (previous != id)
    {
        caustica::info("RtPipelineCache: bound feature preset '%s'", ptFeaturePresetName(id).data());
        m_invalidation->AccumulationResetRequested = true;
    }
    return true;
}

bool SceneRayTracingResources::ensureFeaturePresetReady(PtFeaturePresetId id, bool showProgress)
{
    if (!m_pipelineCache)
        return false;

    // Single CreateStateObject owner: RtPipelineCache::ensureReady / buildPreset.
    if (!m_pipelineCache->ensureReady(id, showProgress))
        return false;
    return bindFeaturePreset(id);
}

void SceneRayTracingResources::clearPipelineBindings()
{
    m_pipelineReference.reset();
    m_pipelineBuildStablePlanes.reset();
    m_pipelineFillStablePlanes.reset();
}

uint32_t SceneRayTracingResources::precacheAllFeaturePresets(bool showProgress)
{
    return m_pipelineCache ? m_pipelineCache->precacheAll(showProgress) : 0;
}

bool SceneRayTracingResources::createBlases(
    caustica::rhi::CommandList* commandList,
    const caustica::scene::SceneRenderData& renderData,
    const PathTracerSettings& settings)
{
    const caustica::AccelStructBuildSettings buildSettings = {
        .excludeTransmissive = settings.AS.ExcludeTransmissive
    };
    m_accelStructs->bindMaterialGpuCache(m_lightingPasses->materials().get());
    return m_accelStructs->createBlases(
        commandList, renderData.staticData().meshSnapshots, buildSettings);
}

void SceneRayTracingResources::uploadSubInstanceData(caustica::rhi::CommandList* commandList)
{
    m_accelStructs->uploadSubInstanceData(commandList);
}

bool SceneRayTracingResources::createTlas(
    caustica::rhi::CommandList* commandList,
    const caustica::scene::SceneRenderData& renderData)
{
    return m_accelStructs->createTlas(commandList, renderData);
}

bool SceneRayTracingResources::createAccelStructs(
    caustica::rhi::CommandList* commandList,
    caustica::Scene& scene,
    const PathTracerSettings& settings,
    const caustica::scene::SceneRenderData* renderData)
{
    (void)scene;
    assert(renderData && "createAccelStructs requires published SceneRenderData");
    const caustica::scene::SceneRenderData& data = *renderData;
    m_lightingPasses->createOpacityMicromaps(data);
    if (!createBlases(commandList, data, settings))
        return false;
    if (!createTlas(commandList, data))
        return false;
    if (m_additionalAccelStructBuilder)
        m_additionalAccelStructBuilder(commandList);
    return m_gpuDevice && m_gpuDevice->getDevice()->isDeviceHealthy();
}

bool SceneRayTracingResources::recreateAccelStructs(
    caustica::rhi::CommandList* commandList,
    caustica::Scene& scene,
    const PathTracerSettings& settings,
    const caustica::scene::SceneRenderData* renderData)
{
    if (!m_invalidation->AccelerationStructRebuildRequested)
        return true;

    m_invalidation->AccelerationStructRebuildRequested = false;
    m_invalidation->AccumulationResetRequested = true;

    assert(renderData && "recreateAccelStructs requires published SceneRenderData");

    // Double-buffered rebuild: keep the previous TLAS/BLAS generation alive in
    // AccelStructManager retired lists so in-flight DispatchRays can finish while
    // this CL builds the new generation. No device-wide waitForIdle — previous
    // frame GPU work overlaps with the AS build on the graphics queue.
    m_accelStructs->clearRetiredAccelStructs();
    m_sceneBindings->invalidate();

    if (!commandList || !commandList->open())
    {
        m_invalidation->AccelerationStructRebuildRequested = true;
        return false;
    }
    if (!createAccelStructs(commandList, scene, settings, renderData))
    {
        commandList->close();
        m_invalidation->AccelerationStructRebuildRequested = true;
        return false;
    }
    commandList->close();
    caustica::rhi::Device* rhiDevice = m_gpuDevice->getDevice();
    const uint64_t submission = rhiDevice->executeCommandList(commandList);
    if ((rhiDevice->getGraphicsAPI() == caustica::rhi::GraphicsAPI::D3D12 && submission == 0)
        || !rhiDevice->isDeviceHealthy())
    {
        m_invalidation->AccelerationStructRebuildRequested = true;
        return false;
    }
    // Binding-set recreate (caller) points at the new TLAS; retired handles keep
    // the old generation valid until the next structure rebuild clears them.
    return rhiDevice->isDeviceHealthy();
}

bool SceneRayTracingResources::recreateAccelStructsForLoad(
    caustica::Scene& scene,
    const caustica::scene::SceneRenderData& renderData,
    const PathTracerSettings& pathTracerSettings,
    uint64_t targetScratchBytesPerSubmit,
    AccelBuildProgress progress)
{
    (void)scene;
    if (!m_invalidation->AccelerationStructRebuildRequested)
        return true;

    caustica::rhi::Device* rhiDevice = m_gpuDevice ? m_gpuDevice->getDevice() : nullptr;
    if (!rhiDevice || !rhiDevice->isDeviceHealthy())
        return false;

    m_invalidation->AccelerationStructRebuildRequested = false;
    m_invalidation->AccumulationResetRequested = true;
    m_accelStructs->clearRetiredAccelStructs();
    m_sceneBindings->invalidate();
    if (progress)
        progress("opacity", 0, renderData.staticData().meshSnapshots.size(), 0);
    m_lightingPasses->createOpacityMicromaps(renderData);
    if (progress)
        progress("blas", 0, renderData.staticData().meshSnapshots.size(), 0);

    constexpr size_t kMaxInFlightScratchBytes = 768ull * 1024ull * 1024ull;
    constexpr uint32_t kMaxInFlightSubmits = 3;
    StreamingUploadBudget buildBudget(kMaxInFlightScratchBytes, kMaxInFlightSubmits);
    const uint64_t batchTarget = std::max<uint64_t>(targetScratchBytesPerSubmit, 1);
    const AccelStructBuildSettings settings = {
        .excludeTransmissive = pathTracerSettings.AS.ExcludeTransmissive
    };

    for (size_t begin = 0; begin < renderData.staticData().meshSnapshots.size();)
    {
        BlasBuildBatchPlan plan;
        if (!m_accelStructs->planBlasBatch(
                renderData.staticData().meshSnapshots, settings, begin, batchTarget, plan)
            || plan.endIndex <= begin)
        {
            m_invalidation->AccelerationStructRebuildRequested = true;
            return false;
        }

        const size_t trackedScratch = static_cast<size_t>(std::max<uint64_t>(plan.scratchBytes, 1));
        if (!buildBudget.waitForBudget(rhiDevice, trackedScratch))
        {
            m_invalidation->AccelerationStructRebuildRequested = true;
            return false;
        }

        caustica::rhi::CommandListParameters params;
        // The planner owns the byte policy. Leaving the per-list hard cap open
        // also permits one legitimate BLAS larger than the normal batch target.
        params.scratchMaxMemory = 0;
        caustica::rhi::CommandListHandle commandList = rhiDevice->createCommandList(params);
        if (!commandList || !commandList->open())
        {
            m_invalidation->AccelerationStructRebuildRequested = true;
            return false;
        }

        const auto batch = std::span<const caustica::scene::MeshRenderResourceSnapshot>(
            renderData.staticData().meshSnapshots.data() + begin,
            plan.endIndex - begin);
        if (!m_accelStructs->createBlases(commandList, batch, settings))
        {
            commandList->close();
            m_invalidation->AccelerationStructRebuildRequested = true;
            return false;
        }

        commandList->close();
        const uint64_t submission = rhiDevice->executeCommandList(commandList);
        if ((rhiDevice->getGraphicsAPI() == caustica::rhi::GraphicsAPI::D3D12 && submission == 0)
            || !rhiDevice->isDeviceHealthy())
        {
            m_invalidation->AccelerationStructRebuildRequested = true;
            return false;
        }
        buildBudget.trackSubmit(rhiDevice, trackedScratch);
        begin = plan.endIndex;
        if (progress)
            progress("blas", begin, renderData.staticData().meshSnapshots.size(), plan.scratchBytes);
    }

    // TLAS storage is allocated only after every BLAS batch is fence-complete.
    if (progress)
        progress("tlas", renderData.staticData().meshSnapshots.size(), renderData.staticData().meshSnapshots.size(), 0);
    caustica::rhi::CommandListHandle finalList = rhiDevice->createCommandList();
    if (!finalList || !finalList->open())
    {
        m_invalidation->AccelerationStructRebuildRequested = true;
        return false;
    }
    if (!createTlas(finalList, renderData))
    {
        finalList->close();
        m_invalidation->AccelerationStructRebuildRequested = true;
        return false;
    }
    if (m_additionalAccelStructBuilder)
        m_additionalAccelStructBuilder(finalList);
    finalList->close();
    const uint64_t finalSubmission = rhiDevice->executeCommandList(finalList);
    if ((rhiDevice->getGraphicsAPI() == caustica::rhi::GraphicsAPI::D3D12 && finalSubmission == 0)
        || !rhiDevice->isDeviceHealthy())
    {
        m_invalidation->AccelerationStructRebuildRequested = true;
        return false;
    }
    buildBudget.trackSubmit(rhiDevice, 1);
    if (progress)
        progress("fence", renderData.staticData().meshSnapshots.size(), renderData.staticData().meshSnapshots.size(), 0);
    if (!buildBudget.waitAll(rhiDevice))
    {
        m_invalidation->AccelerationStructRebuildRequested = true;
        return false;
    }
    if (progress)
        progress("complete", renderData.staticData().meshSnapshots.size(), renderData.staticData().meshSnapshots.size(), 0);
    return rhiDevice->isDeviceHealthy() && m_accelStructs->hasTopLevelAS();
}

void SceneRayTracingResources::requestMeshAccelRebuild(const std::shared_ptr<caustica::MeshInfo>& mesh, bool resetAccumulation)
{
    if (!mesh)
        return;

    if (resetAccumulation)
        m_invalidation->AccumulationResetRequested = true;

    if (!m_accelStructs->hasTopLevelAS())
    {
        m_invalidation->AccelerationStructRebuildRequested = true;
        return;
    }

    m_accelStructs->requestMeshRebuild(RenderResourceAccess::meshId(mesh.get()));
}

void SceneRayTracingResources::requestAccelerationStructureRebuild()
{
    m_invalidation->AccelerationStructRebuildRequested = true;
    m_invalidation->AccumulationResetRequested = true;
}

void SceneRayTracingResources::requestFullRebuild()
{
    m_invalidation->AccelerationStructRebuildRequested = true;
    m_invalidation->ShaderReloadRequested = true;
    m_invalidation->ShaderAndACRefreshDelayedRequest = 0.0f;
    m_invalidation->AccumulationResetRequested = true;
    m_sceneBindings->invalidate();
    if (m_bindingCache)
        m_bindingCache->clear();
}

bool SceneRayTracingResources::consumeShaderReloadRequest()
{
    if (!m_invalidation->ShaderReloadRequested)
        return false;
    m_invalidation->ShaderReloadRequested = false;
    return true;
}

bool SceneRayTracingResources::consumeAccumulationResetRequest()
{
    if (!m_invalidation->AccumulationResetRequested)
        return false;
    m_invalidation->AccumulationResetRequested = false;
    return true;
}

bool& SceneRayTracingResources::accelerationStructRebuildRequested()
{
    return m_invalidation->AccelerationStructRebuildRequested;
}

} // namespace caustica::render

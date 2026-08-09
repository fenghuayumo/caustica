#include <render/SceneRayTracingResources.h>

#include <render/PathTracerScenePasses.h>
#include <render/SceneLightingPasses.h>
#include <render/WorldRenderer.h>

#include <backend/GpuDevice.h>
#include <core/log.h>
#include <render/core/BindingCache.h>
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

bool SceneRayTracingResources::createBlases(
    caustica::rhi::CommandList* commandList,
    const caustica::scene::SceneRenderData& renderData)
{
    caustica::AccelStructBuildSettings settings = { .excludeTransmissive = m_settings->AS.ExcludeTransmissive };
    m_accelStructs->bindMaterialGpuCache(m_lightingPasses->materials().get());
    return m_accelStructs->createBlases(commandList, renderData.staticData().meshSnapshots, settings);
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
    const caustica::scene::SceneRenderData* renderData)
{
    (void)scene;
    assert(renderData && "createAccelStructs requires published SceneRenderData");
    const caustica::scene::SceneRenderData& data = *renderData;
    m_lightingPasses->createOpacityMicromaps(data);
    if (!createBlases(commandList, data))
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
    const caustica::scene::SceneRenderData* renderData)
{
    if (!m_invalidation->AccelerationStructRebuildRequested)
        return true;

    m_invalidation->AccelerationStructRebuildRequested = false;
    m_settings->ResetAccumulation = true;

    assert(renderData && "recreateAccelStructs requires published SceneRenderData");

    // Double-buffered rebuild: keep the previous TLAS/BLAS generation alive in
    // AccelStructManager retired lists so in-flight DispatchRays can finish while
    // this CL builds the new generation. No device-wide waitForIdle — previous
    // frame GPU work overlaps with the AS build on the graphics queue.
    m_accelStructs->clearRetiredAccelStructs();
    m_worldRenderer->invalidateBindingSet();

    if (!commandList || !commandList->open())
    {
        m_invalidation->AccelerationStructRebuildRequested = true;
        return false;
    }
    if (!createAccelStructs(commandList, scene, renderData))
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
    m_settings->ResetAccumulation = true;
    m_accelStructs->clearRetiredAccelStructs();
    m_worldRenderer->invalidateBindingSet();
    if (progress)
        progress("opacity", 0, renderData.staticData().meshSnapshots.size(), 0);
    m_lightingPasses->createOpacityMicromaps(renderData);
    if (progress)
        progress("blas", 0, renderData.staticData().meshSnapshots.size(), 0);

    constexpr size_t kMaxInFlightScratchBytes = 768ull * 1024ull * 1024ull;
    constexpr uint32_t kMaxInFlightSubmits = 3;
    StreamingUploadBudget buildBudget(kMaxInFlightScratchBytes, kMaxInFlightSubmits);
    const uint64_t batchTarget = std::max<uint64_t>(targetScratchBytesPerSubmit, 1);
    const AccelStructBuildSettings settings = { .excludeTransmissive = m_settings->AS.ExcludeTransmissive };

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
        m_settings->ResetAccumulation = true;

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

std::shared_ptr<PTPipelineVariant>& SceneRayTracingResources::pipelineEdgeDetection()
{
    return m_worldRenderer->ptPipelineEdgeDetection();
}

} // namespace caustica::render

#include <render/core/SceneGeometryUpdate.h>
#include <render/core/AccelStructManager.h>
#include <render/core/SceneGpuUpdater.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <render/SceneGpuResources.h>
#include <scene/Scene.h>
#include <scene/SceneRenderData.h>
#include <scene/ResourceTracker.h>

#include <cassert>

namespace caustica
{

namespace
{

AccelStructBuildSettings makeAccelBuildSettings(const PathTracerSettings& settings, bool includeForceOpaque)
{
    AccelStructBuildSettings result = { .excludeTransmissive = settings.AS.ExcludeTransmissive };
    if (includeForceOpaque)
        result.forceOpaque = settings.AS.ForceOpaque;
    return result;
}

OmmAccelStructState makeOmmAccelState(OpacityMicromapBuilder* opacityMicromapBuilder)
{
    OmmAccelStructState ommState = {};
    if (opacityMicromapBuilder == nullptr)
        return ommState;

    const auto& ommUI = opacityMicromapBuilder->uiData();
    ommState.enabled = ommUI.Enable;
    ommState.force2State = ommUI.Force2State;
    ommState.onlyOMMs = ommUI.OnlyOMMs;
    ommState.debugViewEnabled = ommUI.DebugView != OpacityMicroMapDebugView::Disabled;
    return ommState;
}

void transitionSkinnedMeshBuffersToReadOnly(
    nvrhi::ICommandList* commandList,
    const scene::SceneRenderData& renderData,
    render::SceneGpuResources& gpuResources)
{
    for (const scene::SkinnedMeshRenderProxy& proxy : renderData.skinnedMeshes)
    {
        const auto meshGpuIt = gpuResources.meshRegistry.find(proxy.meshId);
        if (meshGpuIt == gpuResources.meshRegistry.end() || !meshGpuIt->second.vertexBuffer)
            continue;
        commandList->setBufferState(
            meshGpuIt->second.vertexBuffer,
            nvrhi::ResourceStates::ShaderResource);
    }
    commandList->commitBarriers();
}

const scene::SceneRenderData& resolveRenderData(const UpdateSceneGeometryParams& params)
{
    assert(params.renderData && "updateSceneGeometry requires published SceneRenderData");
    return *params.renderData;
}

} // namespace

void updateSceneGeometry(AccelStructManager& accelStructs, UpdateSceneGeometryParams& params)
{
    nvrhi::ICommandList* commandList = params.commandList;
    const std::shared_ptr<Scene>& scene = params.scene;
    if (scene == nullptr)
        return;

    const scene::SceneRenderData& renderData = resolveRenderData(params);
    render::SceneGpuResources* gpuResources = params.gpuResources;
    if (gpuResources == nullptr)
        return;

    if (params.materials != nullptr)
        params.materials->ensureMaterialsFromScene(renderData.materialSnapshots);

    render::SceneGpuUpdater::refresh(
        *scene,
        *gpuResources,
        params.descriptorTable,
        commandList,
        static_cast<uint32_t>(params.frameIndex));

    if (params.opacityMaps != nullptr)
        params.opacityMaps->buildOpacityMicromaps(*commandList, renderData);

    const AccelStructBuildSettings rebuildSettings = makeAccelBuildSettings(params.settings, false);
    accelStructs.rebuildDirtyMeshes(
        commandList, renderData, rebuildSettings, params.accelStructRebuildRequested);

    accelStructs.updateSkinnedBlases(
        commandList, renderData, rebuildSettings, static_cast<uint32_t>(params.frameIndex));

    commandList->compactBottomLevelAccelStructs();

    const AccelStructBuildSettings tlasSettings = makeAccelBuildSettings(params.settings, true);
    accelStructs.buildTlas(
        commandList, renderData, tlasSettings, makeOmmAccelState(params.opacityMaps), params.opacityMaps);

    transitionSkinnedMeshBuffersToReadOnly(commandList, renderData, *gpuResources);

    if (params.opacityMaps != nullptr)
    {
        if (params.asyncLoadingInProgress != nullptr)
        {
            *params.asyncLoadingInProgress |= params.opacityMaps->update(*commandList, renderData);
            *params.asyncLoadingInProgress |= params.opacityMaps->uiData().BuildsLeftInQueue > 0;
        }
        else
        {
            (void)params.opacityMaps->update(*commandList, renderData);
        }
    }

    if (params.materials != nullptr)
        params.materials->update(commandList, renderData, gpuResources, accelStructs.getSubInstanceData());

    accelStructs.uploadSubInstanceData(commandList);
}

} // namespace caustica

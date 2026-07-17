#include <render/core/SceneGeometryUpdate.h>
#include <render/core/AccelStructManager.h>
#include <render/core/SceneGpuUpdater.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <render/SceneGpuResources.h>
#include <scene/Scene.h>
#include <scene/SceneRenderData.h>
#include <scene/ResourceTracker.h>

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
    const scene::SceneRenderData& renderData)
{
    for (const scene::SkinnedMeshRenderProxy& proxy : renderData.skinnedMeshes)
    {
        if (!proxy.mesh || !proxy.mesh->buffers || !proxy.mesh->buffers->vertexBuffer)
            continue;
        commandList->setBufferState(
            proxy.mesh->buffers->vertexBuffer,
            nvrhi::ResourceStates::ShaderResource);
    }
    commandList->commitBarriers();
}

const scene::SceneRenderData& resolveRenderData(const UpdateSceneGeometryParams& params)
{
    if (params.renderData)
        return *params.renderData;
    return params.scene->getRenderData();
}

const render::SceneGpuResources* resolveGpuResources(const UpdateSceneGeometryParams& params)
{
    if (params.gpuResources)
        return params.gpuResources;
    return params.scene ? &params.scene->getGpuResources() : nullptr;
}

} // namespace

void updateSceneGeometry(AccelStructManager& accelStructs, UpdateSceneGeometryParams& params)
{
    nvrhi::ICommandList* commandList = params.commandList;
    const std::shared_ptr<Scene>& scene = params.scene;
    if (scene == nullptr)
        return;

    const scene::SceneRenderData& renderData = resolveRenderData(params);
    const render::SceneGpuResources* gpuResources = resolveGpuResources(params);

    render::SceneGpuUpdater::refresh(
        *scene,
        params.descriptorTable,
        commandList,
        static_cast<uint32_t>(params.frameIndex));

    const ResourceTracker<MeshInfo>& meshes = scene->getMeshes();
    const size_t geometryCount = scene->getGeometryCount();

    if (params.opacityMaps != nullptr)
        params.opacityMaps->buildOpacityMicromaps(*commandList, meshes, geometryCount);

    const AccelStructBuildSettings rebuildSettings = makeAccelBuildSettings(params.settings, false);
    accelStructs.rebuildDirtyMeshes(
        commandList, rebuildSettings, params.accelStructRebuildRequested);

    accelStructs.updateSkinnedBlases(
        commandList, renderData, rebuildSettings, static_cast<uint32_t>(params.frameIndex));

    commandList->compactBottomLevelAccelStructs();

    const AccelStructBuildSettings tlasSettings = makeAccelBuildSettings(params.settings, true);
    accelStructs.buildTlas(
        commandList, renderData, tlasSettings, makeOmmAccelState(params.opacityMaps), params.opacityMaps);

    transitionSkinnedMeshBuffersToReadOnly(commandList, renderData);

    if (params.opacityMaps != nullptr)
    {
        if (params.asyncLoadingInProgress != nullptr)
        {
            *params.asyncLoadingInProgress |= params.opacityMaps->update(*commandList, meshes, geometryCount);
            *params.asyncLoadingInProgress |= params.opacityMaps->uiData().BuildsLeftInQueue > 0;
        }
        else
        {
            (void)params.opacityMaps->update(*commandList, meshes, geometryCount);
        }
    }

    if (params.materials != nullptr)
        params.materials->update(commandList, renderData, gpuResources, accelStructs.getSubInstanceData());

    accelStructs.uploadSubInstanceData(commandList);
}

} // namespace caustica

#include <render/Core/SceneGeometryUpdate.h>
#include <render/Core/AccelStructManager.h>
#include <render/Core/SceneGpuUpdater.h>
#include <render/Passes/Lighting/MaterialGpuCache.h>
#include <render/Passes/OMM/OpacityMicromapBuilder.h>
#include <scene/Scene.h>
#include <scene/SceneRenderData.h>

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

    const auto& ommUI = opacityMicromapBuilder->UIData();
    ommState.enabled = ommUI.Enable;
    ommState.force2State = ommUI.Force2State;
    ommState.onlyOMMs = ommUI.OnlyOMMs;
    ommState.debugViewEnabled = ommUI.DebugView != OpacityMicroMapDebugView::Disabled;
    return ommState;
}

void transitionSkinnedMeshBuffersToReadOnly(nvrhi::ICommandList* commandList, const Scene& scene)
{
    for (const scene::SkinnedMeshRenderProxy& proxy : scene.GetRenderData().skinnedMeshes)
    {
        if (!proxy.meshInstance || !proxy.meshInstance->mesh || !proxy.meshInstance->mesh->buffers)
            continue;
        commandList->setBufferState(
            proxy.meshInstance->mesh->buffers->vertexBuffer,
            nvrhi::ResourceStates::ShaderResource);
    }
    commandList->commitBarriers();
}

} // namespace

void updateSceneGeometry(AccelStructManager& accelStructs, UpdateSceneGeometryParams& params)
{
    nvrhi::ICommandList* commandList = params.commandList;
    const std::shared_ptr<Scene>& scene = params.scene;
    if (scene == nullptr)
        return;

    render::SceneGpuUpdater::Refresh(*scene, commandList, static_cast<uint32_t>(params.frameIndex));

    if (params.opacityMaps != nullptr)
        params.opacityMaps->BuildOpacityMicromaps(*commandList, *scene);

    const AccelStructBuildSettings rebuildSettings = makeAccelBuildSettings(params.settings, false);
    accelStructs.rebuildDirtyMeshes(
        commandList, *scene, rebuildSettings, params.accelStructRebuildRequested);

    accelStructs.updateSkinnedBlases(
        commandList, *scene, rebuildSettings, static_cast<uint32_t>(params.frameIndex));

    commandList->compactBottomLevelAccelStructs();

    const AccelStructBuildSettings tlasSettings = makeAccelBuildSettings(params.settings, true);
    accelStructs.buildTlas(
        commandList, *scene, tlasSettings, makeOmmAccelState(params.opacityMaps), params.opacityMaps);

    transitionSkinnedMeshBuffersToReadOnly(commandList, *scene);

    if (params.opacityMaps != nullptr)
    {
        if (params.asyncLoadingInProgress != nullptr)
        {
            *params.asyncLoadingInProgress |= params.opacityMaps->Update(*commandList, *scene);
            *params.asyncLoadingInProgress |= params.opacityMaps->UIData().BuildsLeftInQueue > 0;
        }
        else
        {
            (void)params.opacityMaps->Update(*commandList, *scene);
        }
    }

    if (params.materials != nullptr)
        params.materials->Update(commandList, scene, accelStructs.getSubInstanceData());

    accelStructs.uploadSubInstanceData(commandList);
}

} // namespace caustica

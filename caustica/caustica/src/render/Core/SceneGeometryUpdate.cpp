#include <render/Core/SceneGeometryUpdate.h>
#include <render/Core/RenderCore.h>
#include <render/Core/AccelStructManager.h>
#include <render/Passes/Lighting/MaterialGpuCache.h>
#include <render/Passes/OMM/OpacityMicromapBuilder.h>
#include <scene/Scene.h>
#include <scene/SceneGraph.h>

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
    for (const auto& skinnedInstance : scene.GetSceneGraph()->GetSkinnedMeshInstances())
    {
        commandList->setBufferState(
            skinnedInstance->GetMesh()->buffers->vertexBuffer,
            nvrhi::ResourceStates::ShaderResource);
    }
    commandList->commitBarriers();
}

} // namespace

void RenderCore::updateSceneGeometry(UpdateSceneGeometryParams& params)
{
    nvrhi::ICommandList* commandList = params.commandList;
    const std::shared_ptr<Scene>& scene = params.scene;
    if (scene == nullptr)
        return;

    scene->Refresh(commandList, static_cast<uint32_t>(params.frameIndex));

    if (params.opacityMicromapBuilder != nullptr)
        params.opacityMicromapBuilder->BuildOpacityMicromaps(*commandList, *scene);

    const AccelStructBuildSettings rebuildSettings = makeAccelBuildSettings(params.settings, false);
    m_accelStructs.rebuildDirtyMeshes(
        commandList, *scene, rebuildSettings, params.accelStructRebuildRequested);

    m_accelStructs.updateSkinnedBlases(
        commandList, *scene, rebuildSettings, static_cast<uint32_t>(params.frameIndex));

    commandList->compactBottomLevelAccelStructs();

    const AccelStructBuildSettings tlasSettings = makeAccelBuildSettings(params.settings, true);
    m_accelStructs.buildTlas(
        commandList, *scene, tlasSettings, makeOmmAccelState(params.opacityMicromapBuilder), params.opacityMicromapBuilder);

    transitionSkinnedMeshBuffersToReadOnly(commandList, *scene);

    if (params.opacityMicromapBuilder != nullptr)
    {
        if (params.asyncLoadingInProgress != nullptr)
        {
            *params.asyncLoadingInProgress |= params.opacityMicromapBuilder->Update(*commandList, *scene);
            *params.asyncLoadingInProgress |= params.opacityMicromapBuilder->UIData().BuildsLeftInQueue > 0;
        }
        else
        {
            (void)params.opacityMicromapBuilder->Update(*commandList, *scene);
        }
    }

    if (params.materialGpuCache != nullptr)
        params.materialGpuCache->Update(commandList, scene, m_accelStructs.getSubInstanceData());

    m_accelStructs.uploadSubInstanceData(commandList);
}

} // namespace caustica

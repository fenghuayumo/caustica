#include <render/Core/SceneGeometryUpdate.h>
#include <render/Core/RenderCore.h>
#include <render/Core/AccelStructManager.h>
#include <render/Passes/Lighting/MaterialsBaker.h>
#include <render/Passes/OMM/OmmBaker.h>
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

OmmAccelStructState makeOmmAccelState(OmmBaker* ommBaker)
{
    OmmAccelStructState ommState = {};
    if (ommBaker == nullptr)
        return ommState;

    const auto& ommUI = ommBaker->UIData();
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

    if (params.ommBaker != nullptr)
        params.ommBaker->BuildOpacityMicromaps(*commandList, *scene);

    const AccelStructBuildSettings rebuildSettings = makeAccelBuildSettings(params.settings, false);
    m_accelStructs.rebuildDirtyMeshes(
        commandList, *scene, rebuildSettings, params.accelStructRebuildRequested);

    m_accelStructs.updateSkinnedBlases(
        commandList, *scene, rebuildSettings, static_cast<uint32_t>(params.frameIndex));

    commandList->compactBottomLevelAccelStructs();

    const AccelStructBuildSettings tlasSettings = makeAccelBuildSettings(params.settings, true);
    m_accelStructs.buildTlas(
        commandList, *scene, tlasSettings, makeOmmAccelState(params.ommBaker), params.ommBaker);

    transitionSkinnedMeshBuffersToReadOnly(commandList, *scene);

    if (params.ommBaker != nullptr)
    {
        if (params.asyncLoadingInProgress != nullptr)
        {
            *params.asyncLoadingInProgress |= params.ommBaker->Update(*commandList, *scene);
            *params.asyncLoadingInProgress |= params.ommBaker->UIData().BuildsLeftInQueue > 0;
        }
        else
        {
            (void)params.ommBaker->Update(*commandList, *scene);
        }
    }

    if (params.materialsBaker != nullptr)
        params.materialsBaker->Update(commandList, scene, m_accelStructs.getSubInstanceData());

    m_accelStructs.uploadSubInstanceData(commandList);
}

} // namespace caustica

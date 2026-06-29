#include "PathTracerSessionHost.h"

#include "EditorSessionHost.h"
#include "PathTracerSessionBootstrap.h"
#include "SceneEditor.h"

#include "common/LocalConfig.h"

#include <core/path_utils.h>
#include <engine/EngineRenderer.h>
#include <render/RenderSessionState.h>
#include <render/SceneLightingPasses.h>

namespace caustica::editor
{

void prepareSceneEditorGpuSession(SceneEditor& sceneEditor, caustica::GpuDevice& gpuDevice)
{
    sceneEditor.setGpuDevice(gpuDevice);
    sceneEditor.initStreamlineAndWindow();
}

std::unique_ptr<caustica::EngineRenderer> startupPathTracerSessionHost(
    const PathTracerSessionHostParams& params)
{
    prepareSceneEditorGpuSession(params.sceneEditor, params.gpuDevice);

    auto engineRenderer = bootstrapPathTracerSession(PathTracerSessionBootstrapParams{
        .gpuDevice = params.gpuDevice,
        .sceneEditor = params.sceneEditor,
        .diagnostics = params.diagnostics,
        .frameExtensions = params.frameExtensions,
        .preferredScene = params.preferredScene,
    });

    if (params.refreshEnvMapMediaList)
    {
        engineRenderer->lightingPasses().refreshEnvironmentMapMediaList(
            GetLocalPath(c_AssetsFolder), std::filesystem::path());
    }

    if (params.syncBackBuffer && params.frameDriver)
        syncPathTracerSessionBackBuffer(params.gpuDevice, *params.frameDriver);

    if (params.sessionState && params.cmdLine && params.applyCmdLineToSessionState)
        caustica::render::InitializeRenderSessionStateFromCommandLine(*params.sessionState, *params.cmdLine);

    if (params.sessionState && params.postAppInit)
        LocalConfig::PostAppInit(*params.sessionState);

    return engineRenderer;
}

} // namespace caustica::editor

#include "EditorSceneSubsystem.h"

#include "SceneEditor.h"
#include "common/LocalConfig.h"

#include <render/RenderSessionState.h>

namespace caustica::editor
{

caustica::SceneRuntimeSubsystemConfig EditorSceneSubsystemConfig::toRuntimeConfig() const
{
    return caustica::SceneRuntimeSubsystemConfig{
        .sceneRuntime = sceneEditor,
        .diagnostics = diagnostics,
        .preferredScene = preferredScene,
        .sessionState = sessionState,
        .cmdLine = cmdLine,
        .refreshEnvMapMediaList = refreshEnvMapMediaList,
        .applyCmdLineToSessionState = applyCmdLineToSessionState,
    };
}

EditorSceneSubsystem::EditorSceneSubsystem(EditorSceneSubsystemConfig config)
    : SceneRuntimeSubsystem(config.toRuntimeConfig())
    , m_editorConfig(std::move(config))
{
}

void EditorSceneSubsystem::onInitializePost(caustica::EngineInitContext& /*context*/)
{
    if (m_editorConfig.sessionState && m_editorConfig.postAppInit)
        LocalConfig::PostAppInit(*m_editorConfig.sessionState);
}

void EditorSceneSubsystem::onBeforeBeginFrame()
{
    sceneEditor().CaptureScriptPreRender();
}

void EditorSceneSubsystem::prepareSceneFrame()
{
    sceneEditor().PrepareEditorFrame();
}

} // namespace caustica::editor

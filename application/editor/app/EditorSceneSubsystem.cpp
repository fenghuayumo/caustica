#include "EditorSceneSubsystem.h"

#include "SceneEditor.h"
#include "common/LocalConfig.h"

#include <render/RenderSessionState.h>

namespace caustica::editor
{

EditorSceneSubsystem::EditorSceneSubsystem(EditorSceneSubsystemConfig config)
    : SceneRuntimeSubsystem(std::move(config.runtime))
    , m_postAppInit(config.postAppInit)
{
}

void EditorSceneSubsystem::onInitializePost(caustica::EngineInitContext& /*context*/)
{
    if (m_config.sessionState && m_postAppInit)
        LocalConfig::PostAppInit(*m_config.sessionState);
}

void EditorSceneSubsystem::onBeforeBeginFrame()
{
    sceneEditor().CaptureScriptPreRender();
}

void EditorSceneSubsystem::prepareSceneFrame()
{
    sceneEditor().PrepareEditorFrame();
}

SceneEditor& EditorSceneSubsystem::sceneEditor()
{
    return static_cast<SceneEditor&>(m_config.sceneRuntime);
}

const SceneEditor& EditorSceneSubsystem::sceneEditor() const
{
    return static_cast<const SceneEditor&>(m_config.sceneRuntime);
}

} // namespace caustica::editor

#include "EditorSceneSubsystem.h"

#include "SceneEditor.h"
#include "common/LocalConfig.h"

#include <engine/GpuRenderSubsystem.h>
#include <engine/SubsystemCollection.h>
#include <render/RenderSessionState.h>

namespace caustica::editor
{

EditorSceneSubsystem::EditorSceneSubsystem(EditorSceneSubsystemConfig config)
    : SceneSessionSubsystem(std::move(config.session))
    , m_sceneEditor(config.sceneEditor)
    , m_postAppInit(config.postAppInit)
{
}

void EditorSceneSubsystem::initialize(caustica::EngineInitContext& context)
{
    if (m_sceneEditor && context.app)
        m_sceneEditor->setApp(*context.app);

    SceneSessionSubsystem::initialize(context);

    if (m_sceneEditor && context.subsystems)
    {
        if (auto* gpuRender = context.subsystems->get<caustica::GpuRenderSubsystem>())
            m_sceneEditor->attachGpuRenderSubsystem(*gpuRender);
    }
}

void EditorSceneSubsystem::onInitializePost(caustica::EngineInitContext& /*context*/)
{
    if (m_config.sessionState && m_postAppInit)
        LocalConfig::PostAppInit(*m_config.sessionState);
}

} // namespace caustica::editor

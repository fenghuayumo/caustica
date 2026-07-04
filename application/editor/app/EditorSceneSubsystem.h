#pragma once

#include <core/command_line.h>
#include <engine/SceneRuntimeSubsystem.h>

namespace caustica::editor
{

class SceneEditor;

struct EditorSceneSubsystemConfig
{
    SceneEditor& sceneEditor;
    caustica::render::SessionDiagnostics& diagnostics;
    std::string preferredScene;

    caustica::render::RenderSessionState* sessionState = nullptr;
    const CommandLineOptions* cmdLine = nullptr;

    bool refreshEnvMapMediaList = true;
    bool applyCmdLineToSessionState = true;
    bool postAppInit = true;

    [[nodiscard]] caustica::SceneRuntimeSubsystemConfig toRuntimeConfig() const;
};

// Editor scene driver: extends SceneRuntimeSubsystem with capture scripts and local config.
class EditorSceneSubsystem : public caustica::SceneRuntimeSubsystem
{
public:
    explicit EditorSceneSubsystem(EditorSceneSubsystemConfig config);

protected:
    void onInitializePost(caustica::EngineInitContext& context) override;
    void onBeforeBeginFrame() override;
    void prepareSceneFrame() override;

private:
    SceneEditor& sceneEditor() { return m_editorConfig.sceneEditor; }
    const SceneEditor& sceneEditor() const { return m_editorConfig.sceneEditor; }

    EditorSceneSubsystemConfig m_editorConfig;
};

} // namespace caustica::editor

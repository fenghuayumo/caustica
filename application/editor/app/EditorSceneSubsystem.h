#pragma once

#include <engine/SceneRuntimeSubsystem.h>

namespace caustica::editor
{

class SceneEditor;

struct EditorSceneSubsystemConfig
{
    caustica::SceneRuntimeSubsystemConfig runtime;
    bool postAppInit = true;
};

// Editor scene driver: extends SceneRuntimeSubsystem with capture scripts and local config.
class EditorSceneSubsystem : public caustica::SceneRuntimeSubsystem
{
public:
    explicit EditorSceneSubsystem(EditorSceneSubsystemConfig config);

protected:
    void onInitializePost(caustica::EngineInitContext& context) override;
    void onBeforeBeginFrame() override;
    void onPrepareRenderScene(caustica::GpuDevice& gpuDevice) override;

private:
    [[nodiscard]] SceneEditor& sceneEditor();
    [[nodiscard]] const SceneEditor& sceneEditor() const;

    bool m_postAppInit = true;
};

} // namespace caustica::editor

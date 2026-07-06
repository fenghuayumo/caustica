#pragma once

#include <core/command_line.h>
#include <engine/App.h>
#include <engine/ISubsystem.h>

#include <memory>

namespace caustica::editor
{

class EditorUI;
class EditorUIData;
class SceneEditor;

struct EditorUISubsystemConfig
{
    App& app;
    SceneEditor& sceneEditor;
    EditorUIData& editorUiData;
    const CommandLineOptions& cmdLine;
};

class EditorUISubsystem : public caustica::ISubsystem
{
public:
    explicit EditorUISubsystem(EditorUISubsystemConfig config);

    [[nodiscard]] int priority() const override { return 250; }

    void initialize(caustica::EngineInitContext& context) override;
    void shutdown() override;

    void animateScheduled(float elapsedTimeSeconds, bool windowFocused);
    void renderSceneScheduled(caustica::GpuDevice& gpuDevice);
    void onBackBufferResizing() override;
    void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) override;

    void onDisplayScaleChanged(float scaleX, float scaleY);

private:
    EditorUISubsystemConfig m_config;
    std::unique_ptr<EditorUI> m_ui;
};

} // namespace caustica::editor

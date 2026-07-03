#pragma once

#include <core/command_line.h>
#include <engine/ISubsystem.h>

#include <memory>

namespace caustica::editor
{

class EditorApplication;
class EditorUI;
class EditorUIData;

struct EditorUISubsystemConfig
{
    EditorApplication& editorApplication;
    EditorUIData& editorUiData;
    const CommandLineOptions& cmdLine;
};

// Owns ImGui overlay lifecycle for the desktop editor.
class EditorUISubsystem : public caustica::ISubsystem
{
public:
    explicit EditorUISubsystem(EditorUISubsystemConfig config);

    [[nodiscard]] int priority() const override { return 250; }

    void initialize(caustica::EngineInitContext& context) override;
    void shutdown() override;

    void onUpdate(float elapsedTimeSeconds, bool windowFocused) override;
    void onRenderScene(caustica::GpuDevice& gpuDevice) override;
    void onBackBufferResizing() override;
    void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) override;

    void onDisplayScaleChanged(float scaleX, float scaleY);

private:
    EditorUISubsystemConfig m_config;
    std::unique_ptr<EditorUI> m_ui;
};

} // namespace caustica::editor

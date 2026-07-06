#pragma once

#include <core/command_line.h>
#include <engine/App.h>

#include <memory>

namespace caustica
{
class GpuDevice;
class Window;
}

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

class EditorUISubsystem
{
public:
    explicit EditorUISubsystem(EditorUISubsystemConfig config);
    ~EditorUISubsystem();

    void startup(caustica::GpuDevice& gpuDevice, caustica::Window& window, caustica::App& app);
    void shutdown();

    void animateScheduled(float elapsedTimeSeconds, bool windowFocused);
    void renderSceneScheduled(caustica::GpuDevice& gpuDevice);
    void onBackBufferResizing();
    void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount);

    void onDisplayScaleChanged(float scaleX, float scaleY);

private:
    EditorUISubsystemConfig m_config;
    std::unique_ptr<EditorUI> m_ui;
};

} // namespace caustica::editor

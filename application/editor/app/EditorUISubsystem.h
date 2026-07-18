#pragma once

#include <core/command_line.h>
#include <engine/App.h>

#include <chrono>
#include <cstdint>
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
class EditorViewport;
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
    // Prepare viewport FB + RenderFramebufferOverride before WorldRenderer::render.
    void prepareViewportForRender(caustica::GpuDevice& gpuDevice);
    void renderSceneScheduled(caustica::GpuDevice& gpuDevice);
    void onBackBufferResizing();
    void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount);

    void onDisplayScaleChanged(float scaleX, float scaleY);

private:
    EditorUISubsystemConfig m_config;
    std::unique_ptr<EditorUI> m_ui;
    std::unique_ptr<EditorViewport> m_viewport;

    // Debounce viewport FB recreation while the user is dragging a dock split.
    uint32_t m_pendingViewportWidth = 0;
    uint32_t m_pendingViewportHeight = 0;
    std::chrono::steady_clock::time_point m_viewportSizeChangedAt{};
};

} // namespace caustica::editor

#pragma once

#include <core/command_line.h>
#include <engine/App.h>
#include <backend/SurfaceObserver.h>
#include <rhi/rhi.h>

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
class RenderSettingsConsoleBinding;
class SceneEditor;

struct EditorUISubsystemConfig
{
    App& app;
    SceneEditor& sceneEditor;
    EditorUIData& editorUiData;
    const CommandLineOptions& cmdLine;
    RenderSettingsConsoleBinding& console;
};

class EditorUISubsystem : public ISurfaceObserver
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
    void onSurfaceResizing() override;
    void onSurfaceResized(uint32_t width, uint32_t height, uint32_t sampleCount) override;
    void onDisplayScaleChanged(float scaleX, float scaleY) override;

private:
    EditorUISubsystemConfig m_config;
    std::unique_ptr<EditorUI> m_ui;
    std::unique_ptr<EditorViewport> m_viewport;
    // Persistent compositor list: its backend allocator/list pool is reused
    // across frames instead of allocating a new D3D12 pair for every clear.
    caustica::rhi::CommandListHandle m_compositeCommandList;

    // Debounce viewport FB recreation while the user is dragging a dock split.
    uint32_t m_pendingViewportWidth = 0;
    uint32_t m_pendingViewportHeight = 0;
    std::chrono::steady_clock::time_point m_viewportSizeChangedAt{};
};

} // namespace caustica::editor

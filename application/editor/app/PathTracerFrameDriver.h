#pragma once

#include <engine/Application.h>

namespace caustica
{
class EngineRenderer;
class GpuDevice;
class Window;
}

namespace caustica::editor
{

class SceneEditor;

// Minimal Application frame driver for headless/windowed Python sessions.
class PathTracerFrameDriver : public caustica::Application
{
public:
    PathTracerFrameDriver(caustica::GpuDevice* gpuDevice,
        caustica::Window* window,
        SceneEditor* sceneEditor,
        caustica::EngineRenderer* engineRenderer);

    void syncToBackBuffer();

protected:
    void onBeginFrame(caustica::GpuDevice& gpuDevice) override;
    bool skipRenderPhase() const override;
    void onUpdate(float elapsedTimeSeconds, bool windowFocused) override;
    void onRender() override;
    bool shouldRenderWhenUnfocused() const override;
    void onBackBufferResizing() override;

private:
    SceneEditor* m_sceneEditor = nullptr;
    caustica::EngineRenderer* m_engineRenderer = nullptr;
};

} // namespace caustica::editor

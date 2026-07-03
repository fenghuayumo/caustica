#pragma once

#include <engine/Application.h>
#include <engine/Engine.h>

namespace caustica
{

class GpuDevice;
class Window;

// Application adapter that forwards frame callbacks to an Engine instance.
// Used by the desktop editor, headless Python sessions, and any external GpuDevice owner.
class EngineFrameApplication : public Application
{
public:
    EngineFrameApplication() = default;
    EngineFrameApplication(Engine* engine, GpuDevice* gpuDevice, Window* window = nullptr);

    void setEngine(Engine* engine) { m_engine = engine; }
    void syncSwapChain();

protected:
    void onBeginFrame(GpuDevice& gpuDevice) override;
    bool skipRenderPhase() const override;
    void onUpdate(float elapsedTimeSeconds, bool windowFocused) override;
    void onRender() override;
    bool shouldRenderWhenUnfocused() const override;
    void onBackBufferResizing() override;
    void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) override;

private:
    Engine* m_engine = nullptr;
};

} // namespace caustica

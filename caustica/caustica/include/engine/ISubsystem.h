#pragma once

#include <cstdint>

namespace caustica
{

class Application;
class GpuDevice;
class Window;

struct EngineInitContext
{
    GpuDevice* gpuDevice = nullptr;
    Window* window = nullptr;
    Application* application = nullptr;
    class SubsystemCollection* subsystems = nullptr;
};

// Lifecycle unit registered with caustica::Engine. Lower priority initializes first;
// shutdown runs in reverse order.
class ISubsystem
{
public:
    virtual ~ISubsystem() = default;

    [[nodiscard]] virtual int priority() const { return 0; }

    virtual void initialize(EngineInitContext& context) {}
    virtual void shutdown() {}

    virtual void onBeginFrame(GpuDevice& gpuDevice) {}
    virtual void onUpdate(float elapsedTimeSeconds, bool windowFocused) {}
    virtual void onRenderScene(GpuDevice& gpuDevice) {}
    virtual void onRenderEnd(GpuDevice& gpuDevice) {}

    virtual void onBackBufferResizing() {}
    virtual void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) {}

    [[nodiscard]] virtual bool skipRenderPhase() const { return false; }
    [[nodiscard]] virtual bool shouldRenderWhenUnfocused() const { return false; }
};

} // namespace caustica

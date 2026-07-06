#pragma once

#include <cstdint>

namespace caustica
{

class App;
class GpuDevice;
class Window;

struct EngineInitContext
{
    GpuDevice* gpuDevice = nullptr;
    Window* window = nullptr;
    App* app = nullptr;
    class SubsystemCollection* subsystems = nullptr;
};

// Lifecycle unit registered with Engine. Lower priority initializes first;
// shutdown runs in reverse order.
class ISubsystem
{
public:
    virtual ~ISubsystem() = default;

    [[nodiscard]] virtual int priority() const { return 0; }

    virtual void initialize(EngineInitContext& context) {}
    virtual void shutdown() {}

    virtual void onBackBufferResizing() {}
    virtual void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount) {}
};

} // namespace caustica

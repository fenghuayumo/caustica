#pragma once

#include <engine/ISubsystem.h>
#include <engine/SubsystemCollection.h>

#include <memory>

namespace caustica
{

class Application;
class GpuDevice;

// Unified runtime: owns subsystems and routes Application frame callbacks.
class Engine
{
public:
    Engine() = default;
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void addSubsystem(std::unique_ptr<ISubsystem> subsystem);

    template<typename T>
    [[nodiscard]] T* getSubsystem() const
    {
        return m_subsystems.get<T>();
    }

    [[nodiscard]] SubsystemCollection& subsystems() { return m_subsystems; }
    [[nodiscard]] const SubsystemCollection& subsystems() const { return m_subsystems; }

    bool initialize(EngineInitContext context);
    void shutdown();

    void onBeginFrame(GpuDevice& gpuDevice);
    void onUpdate(float elapsedTimeSeconds, bool windowFocused);
    void onRenderScene(GpuDevice& gpuDevice);
    void onRenderEnd(GpuDevice& gpuDevice);

    void onBackBufferResizing();
    void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount);

    [[nodiscard]] bool skipRenderPhase() const;
    [[nodiscard]] bool shouldRenderWhenUnfocused() const;

    // Notify frame driver that the swap chain matches the current back buffer.
    static void syncSwapChain(GpuDevice& gpuDevice, Application& frameDriver);

private:
    SubsystemCollection m_subsystems;
    bool m_initialized = false;
};

} // namespace caustica

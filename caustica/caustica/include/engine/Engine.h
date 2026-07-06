#pragma once

#include <engine/ISubsystem.h>
#include <engine/SubsystemCollection.h>

#include <memory>

namespace caustica
{

class App;
class GpuDevice;

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

    void onBackBufferResizing();
    void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount);

    static void syncSwapChain(GpuDevice& gpuDevice, App& app);

private:
    SubsystemCollection m_subsystems;
    bool m_initialized = false;
};

} // namespace caustica

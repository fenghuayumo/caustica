#pragma once

#include <engine/ISubsystem.h>

#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace caustica
{

class SubsystemCollection
{
public:
    void add(std::unique_ptr<ISubsystem> subsystem);

    bool initializeAll(EngineInitContext& context);
    void shutdownAll();

    void onBeginFrame(GpuDevice& gpuDevice);
    void onUpdate(float elapsedTimeSeconds, bool windowFocused);
    void onRenderScene(GpuDevice& gpuDevice);
    void onRenderEnd(GpuDevice& gpuDevice);
    void onBackBufferResizing();
    void onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount);

    [[nodiscard]] bool skipRenderPhase() const;
    [[nodiscard]] bool shouldRenderWhenUnfocused() const;

    template<typename T>
    [[nodiscard]] T* get() const
    {
        const auto it = m_byType.find(std::type_index(typeid(T)));
        if (it == m_byType.end())
            return nullptr;
        return static_cast<T*>(it->second);
    }

private:
    std::vector<std::unique_ptr<ISubsystem>> m_subsystems;
    std::unordered_map<std::type_index, ISubsystem*> m_byType;
    bool m_initialized = false;
};

} // namespace caustica

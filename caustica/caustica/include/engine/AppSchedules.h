#pragma once

#include <ecs/World.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace caustica
{

class App;
class GpuDevice;

enum class AppSchedule
{
    Startup,
    First,
    preUpdate,
    update,
    PostUpdate,
    Extract,
    render,
    PostRender,
    Last,
    Shutdown,
    Count,
};

[[nodiscard]] const char* toString(AppSchedule schedule);

struct SystemContext
{
    App& app;
    ecs::World& world;
    GpuDevice* gpuDevice = nullptr;
    float deltaTimeSeconds = 0.0f;
    uint32_t frameIndex = 0;
    bool windowFocused = true;
    bool windowVisible = true;

    double elapsedTime = 0.0;
    double currentTime = 0.0;

    bool runUpdate = false;
    bool runRender = false;
    bool abortFrame = false;

    template<typename T>
    [[nodiscard]] T& resMut()
    {
        return world.resource<T>();
    }

    template<typename T>
    [[nodiscard]] const T& res() const
    {
        return world.resource<T>();
    }

    template<typename T>
    [[nodiscard]] T* tryRes()
    {
        return world.getResource<T>();
    }

    template<typename T>
    [[nodiscard]] const T* tryRes() const
    {
        return world.getResource<T>();
    }

    [[nodiscard]] ecs::CommandQueue& commands()
    {
        return world.commands();
    }
};

using SystemFn = std::function<void(SystemContext&)>;

struct AppSystemOrdering
{
    std::vector<std::string> before;
    std::vector<std::string> after;
};

// Ordered systems for a single AppSchedule phase.
class AppSchedules
{
public:
    AppSchedules& addSystem(
        AppSchedule schedule,
        std::string name,
        SystemFn system,
        AppSystemOrdering ordering = {});
    AppSchedules& addSystemBefore(
        AppSchedule schedule,
        std::string name,
        std::string before,
        SystemFn system);
    AppSchedules& addSystemAfter(
        AppSchedule schedule,
        std::string name,
        std::string after,
        SystemFn system);
    void run(AppSchedule schedule, SystemContext& context) const;
    void clear();

private:
    struct System
    {
        std::string name;
        SystemFn fn;
        AppSystemOrdering ordering;
    };

    struct PhaseSchedule
    {
        std::vector<System> systems;
    };

    [[nodiscard]] static std::size_t phaseIndex(AppSchedule schedule);
    [[nodiscard]] static std::vector<int> buildExecutionOrder(const PhaseSchedule& phase);

    PhaseSchedule m_phases[static_cast<std::size_t>(AppSchedule::Count)];
};

} // namespace caustica

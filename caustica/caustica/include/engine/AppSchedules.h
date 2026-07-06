#pragma once

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
    PreUpdate,
    Update,
    PostUpdate,
    Extract,
    Render,
    PostRender,
    Last,
    Shutdown,
    Count,
};

[[nodiscard]] const char* toString(AppSchedule schedule);

struct AppScheduleContext
{
    App& app;
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
};

using AppSystemFn = std::function<void(AppScheduleContext&)>;

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
        AppSystemFn system,
        AppSystemOrdering ordering = {});
    AppSchedules& addSystemBefore(
        AppSchedule schedule,
        std::string name,
        std::string before,
        AppSystemFn system);
    AppSchedules& addSystemAfter(
        AppSchedule schedule,
        std::string name,
        std::string after,
        AppSystemFn system);
    void run(AppSchedule schedule, AppScheduleContext& context) const;
    void clear();

private:
    struct System
    {
        std::string name;
        AppSystemFn fn;
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

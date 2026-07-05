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
    PreUpdate,
    Update,
    PostUpdate,
    PreRender,
    Render,
    PostRender,
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
};

using AppSystemFn = std::function<void(AppScheduleContext&)>;

// Ordered systems for a single AppSchedule phase.
class AppSchedules
{
public:
    AppSchedules& addSystem(AppSchedule schedule, std::string name, AppSystemFn system);
    void run(AppSchedule schedule, AppScheduleContext& context) const;
    void clear();

private:
    struct System
    {
        std::string name;
        AppSystemFn fn;
    };

    struct PhaseSchedule
    {
        std::vector<System> systems;
    };

    [[nodiscard]] static std::size_t phaseIndex(AppSchedule schedule);

    PhaseSchedule m_phases[7];
};

} // namespace caustica

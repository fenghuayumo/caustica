#include <engine/AppSchedules.h>

namespace caustica
{

const char* toString(AppSchedule schedule)
{
    switch (schedule)
    {
    case AppSchedule::Startup: return "Startup";
    case AppSchedule::PreUpdate: return "PreUpdate";
    case AppSchedule::Update: return "Update";
    case AppSchedule::PostUpdate: return "PostUpdate";
    case AppSchedule::PreRender: return "PreRender";
    case AppSchedule::Render: return "Render";
    case AppSchedule::PostRender: return "PostRender";
    default: return "Unknown";
    }
}

std::size_t AppSchedules::phaseIndex(AppSchedule schedule)
{
    return static_cast<std::size_t>(schedule);
}

AppSchedules& AppSchedules::addSystem(AppSchedule schedule, std::string name, AppSystemFn system)
{
    PhaseSchedule& phase = m_phases[phaseIndex(schedule)];
    phase.systems.push_back(System{ std::move(name), std::move(system) });
    return *this;
}

void AppSchedules::run(AppSchedule schedule, AppScheduleContext& context) const
{
    const PhaseSchedule& phase = m_phases[phaseIndex(schedule)];
    for (const System& system : phase.systems)
    {
        if (system.fn)
            system.fn(context);
    }
}

void AppSchedules::clear()
{
    for (PhaseSchedule& phase : m_phases)
        phase.systems.clear();
}

} // namespace caustica

#include <engine/AppSchedules.h>

#include <algorithm>
#include <utility>

namespace caustica
{

const char* toString(AppSchedule schedule)
{
    switch (schedule)
    {
    case AppSchedule::Startup: return "Startup";
    case AppSchedule::First: return "First";
    case AppSchedule::PreUpdate: return "PreUpdate";
    case AppSchedule::Update: return "Update";
    case AppSchedule::PostUpdate: return "PostUpdate";
    case AppSchedule::Extract: return "Extract";
    case AppSchedule::Render: return "Render";
    case AppSchedule::PostRender: return "PostRender";
    case AppSchedule::Last: return "Last";
    case AppSchedule::Shutdown: return "Shutdown";
    default: return "Unknown";
    }
}

std::size_t AppSchedules::phaseIndex(AppSchedule schedule)
{
    return static_cast<std::size_t>(schedule);
}

AppSchedules& AppSchedules::addSystem(
    AppSchedule schedule,
    std::string name,
    SystemFn system,
    AppSystemOrdering ordering)
{
    PhaseSchedule& phase = m_phases[phaseIndex(schedule)];
    phase.systems.push_back(System{ std::move(name), std::move(system), std::move(ordering) });
    return *this;
}

AppSchedules& AppSchedules::addSystemBefore(
    AppSchedule schedule,
    std::string name,
    std::string before,
    SystemFn system)
{
    AppSystemOrdering ordering;
    ordering.before.push_back(std::move(before));
    return addSystem(schedule, std::move(name), std::move(system), std::move(ordering));
}

AppSchedules& AppSchedules::addSystemAfter(
    AppSchedule schedule,
    std::string name,
    std::string after,
    SystemFn system)
{
    AppSystemOrdering ordering;
    ordering.after.push_back(std::move(after));
    return addSystem(schedule, std::move(name), std::move(system), std::move(ordering));
}

void AddEdge(std::vector<std::vector<int>>& outgoing, std::vector<int>& indegree, int from, int to)
{
    if (from < 0 || to < 0 || from == to)
        return;

    std::vector<int>& edges = outgoing[static_cast<std::size_t>(from)];
    if (std::find(edges.begin(), edges.end(), to) != edges.end())
        return;

    edges.push_back(to);
    ++indegree[static_cast<std::size_t>(to)];
}

std::vector<int> AppSchedules::buildExecutionOrder(const PhaseSchedule& phase)
{
    const std::vector<System>& systems = phase.systems;
    const int count = static_cast<int>(systems.size());
    std::vector<std::vector<int>> outgoing(static_cast<std::size_t>(count));
    std::vector<int> indegree(static_cast<std::size_t>(count), 0);

    auto findSystemIndex = [&systems](const std::string& name) -> int {
        const auto it = std::find_if(systems.begin(), systems.end(), [&name](const System& system) {
            return system.name == name;
        });
        if (it == systems.end())
            return -1;
        return static_cast<int>(std::distance(systems.begin(), it));
    };

    for (int i = 0; i < count; ++i)
    {
        const AppSystemOrdering& ordering = systems[static_cast<std::size_t>(i)].ordering;
        for (const std::string& before : ordering.before)
            AddEdge(outgoing, indegree, i, findSystemIndex(before));
        for (const std::string& after : ordering.after)
            AddEdge(outgoing, indegree, findSystemIndex(after), i);
    }

    std::vector<int> ordered;
    ordered.reserve(systems.size());
    std::vector<bool> consumed(static_cast<std::size_t>(count), false);

    while (static_cast<int>(ordered.size()) < count)
    {
        int next = -1;
        for (int i = 0; i < count; ++i)
        {
            if (!consumed[static_cast<std::size_t>(i)] && indegree[static_cast<std::size_t>(i)] == 0)
            {
                next = i;
                break;
            }
        }

        if (next < 0)
        {
            for (int i = 0; i < count; ++i)
            {
                if (!consumed[static_cast<std::size_t>(i)])
                    ordered.push_back(i);
            }
            break;
        }

        consumed[static_cast<std::size_t>(next)] = true;
        ordered.push_back(next);
        for (int target : outgoing[static_cast<std::size_t>(next)])
            --indegree[static_cast<std::size_t>(target)];
    }

    return ordered;
}

void AppSchedules::run(AppSchedule schedule, SystemContext& context) const
{
    const PhaseSchedule& phase = m_phases[phaseIndex(schedule)];
    const std::vector<int> order = buildExecutionOrder(phase);
    for (int index : order)
    {
        const System& system = phase.systems[static_cast<std::size_t>(index)];
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

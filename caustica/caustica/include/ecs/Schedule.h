#pragma once

#include <ecs/World.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace caustica::ecs
{

struct ScheduleContext
{
    float deltaTimeSeconds = 0.0f;
};

class Schedule
{
public:
    using SystemFn = std::function<void(World&, const ScheduleContext&)>;

    Schedule& addSet(std::string name)
    {
        if (findSet(name) == nullptr)
            m_sets.push_back(SystemSet{ std::move(name), {} });
        return *this;
    }

    Schedule& addSystem(const std::string& setName, std::string systemName, SystemFn system)
    {
        SystemSet* set = findSet(setName);
        if (!set)
        {
            addSet(setName);
            set = &m_sets.back();
        }

        set->systems.push_back(System{ std::move(systemName), std::move(system) });
        return *this;
    }

    void run(World& world, const ScheduleContext& context) const
    {
        for (const SystemSet& set : m_sets)
        {
            for (const System& system : set.systems)
                system.fn(world, context);
        }
    }

    void clear()
    {
        m_sets.clear();
    }

private:
    struct System
    {
        std::string name;
        SystemFn fn;
    };

    struct SystemSet
    {
        std::string name;
        std::vector<System> systems;
    };

    SystemSet* findSet(const std::string& name)
    {
        for (SystemSet& set : m_sets)
        {
            if (set.name == name)
                return &set;
        }
        return nullptr;
    }

    std::vector<SystemSet> m_sets;
};

} // namespace caustica::ecs

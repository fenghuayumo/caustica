#pragma once

#include <ecs/World.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace caustica::ecs
{

struct ScheduleContext
{
    float deltaTimeSeconds = 0.0f;
    uint32_t frameIndex = 0;
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
        m_flatOrderDirty = true;
        return *this;
    }

    // Run `earlier` before `later`.
    Schedule& before(const std::string& earlier, const std::string& later)
    {
        m_orderConstraints.emplace_back(earlier, later);
        m_flatOrderDirty = true;
        return *this;
    }

    // Run `later` after `earlier`.
    Schedule& after(const std::string& later, const std::string& earlier)
    {
        return before(earlier, later);
    }

    void run(World& world, const ScheduleContext& context) const
    {
        rebuildFlatOrderIfNeeded();

        for (const System* system : m_flatOrder)
        {
            if (system && system->fn)
                system->fn(world, context);
        }
    }

    void clear()
    {
        m_sets.clear();
        m_flatOrder.clear();
        m_orderConstraints.clear();
        m_flatOrderDirty = true;
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

    void rebuildFlatOrderIfNeeded() const
    {
        if (!m_flatOrderDirty)
            return;

        m_flatOrder.clear();
        std::vector<System*> registrationOrder;
        std::unordered_map<std::string, System*> systemsByName;

        for (const SystemSet& set : m_sets)
        {
            for (const System& system : set.systems)
            {
                System* mutableSystem = const_cast<System*>(&system);
                registrationOrder.push_back(mutableSystem);
                systemsByName[system.name] = mutableSystem;
            }
        }

        std::unordered_map<std::string, std::vector<std::string>> adjacency;
        std::unordered_map<std::string, int> indegree;
        for (System* system : registrationOrder)
            indegree[system->name] = 0;

        for (const auto& [earlier, later] : m_orderConstraints)
        {
            if (!systemsByName.contains(earlier) || !systemsByName.contains(later))
                continue;

            adjacency[earlier].push_back(later);
            ++indegree[later];
        }

        std::vector<std::string> ready;
        ready.reserve(registrationOrder.size());
        for (System* system : registrationOrder)
        {
            if (indegree[system->name] == 0)
                ready.push_back(system->name);
        }

        std::vector<std::string> sortedNames;
        sortedNames.reserve(registrationOrder.size());
        while (!ready.empty())
        {
            const std::string name = ready.front();
            ready.erase(ready.begin());
            sortedNames.push_back(name);

            for (const std::string& dependent : adjacency[name])
            {
                if (--indegree[dependent] == 0)
                    ready.push_back(dependent);
            }
        }

        if (sortedNames.size() != registrationOrder.size())
        {
            // Cycle detected — fall back to registration order.
            for (System* system : registrationOrder)
                m_flatOrder.push_back(system);
        }
        else
        {
            for (const std::string& name : sortedNames)
                m_flatOrder.push_back(systemsByName[name]);
        }

        m_flatOrderDirty = false;
    }

    std::vector<SystemSet> m_sets;
    std::vector<std::pair<std::string, std::string>> m_orderConstraints;
    mutable std::vector<System*> m_flatOrder;
    mutable bool m_flatOrderDirty = true;
};

} // namespace caustica::ecs

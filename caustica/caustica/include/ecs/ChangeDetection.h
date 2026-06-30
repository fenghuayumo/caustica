#pragma once

#include <ecs/Entity.h>

#include <entt/entity/registry.hpp>

#include <cstdint>

namespace caustica::ecs
{

struct ComponentTicks
{
    uint32_t added = 0;
    uint32_t changed = 0;
};

template<typename T>
struct ComponentChangeTicks
{
    entt::dense_map<Entity, ComponentTicks> ticks;
};

// Tick-based component change tracking (Bevy-style Changed<T> / Added<T>).
// Per-component tick maps live in registry context as ComponentChangeTicks<T>.
class ChangeDetection
{
public:
    [[nodiscard]] uint32_t tick() const { return m_tick; }

    void endFrame() { ++m_tick; }

    void noteStructureChange() { m_worldStructureChanged = true; }
    [[nodiscard]] bool worldStructureChanged() const { return m_worldStructureChanged; }

    void clearWorldStructureChange() { m_worldStructureChanged = false; }

    template<typename T>
    void markAdded(Entity entity, entt::registry& registry)
    {
        auto& storage = storageFor<T>(registry);
        storage.ticks[entity].added = m_tick;
        storage.ticks[entity].changed = m_tick;
    }

    template<typename T>
    void markChanged(Entity entity, entt::registry& registry)
    {
        storageFor<T>(registry).ticks[entity].changed = m_tick;
    }

    template<typename T>
    void markRemoved(Entity entity, entt::registry& registry)
    {
        if (auto* storage = registry.ctx().find<ComponentChangeTicks<T>>())
            storage->ticks.erase(entity);
    }

    template<typename T>
    [[nodiscard]] bool isAddedThisFrame(Entity entity, const entt::registry& registry) const
    {
        const auto* storage = registry.ctx().find<ComponentChangeTicks<T>>();
        if (!storage)
            return false;
        const auto it = storage->ticks.find(entity);
        return it != storage->ticks.end() && it->second.added == m_tick;
    }

    template<typename T>
    [[nodiscard]] bool isChangedThisFrame(Entity entity, const entt::registry& registry) const
    {
        const auto* storage = registry.ctx().find<ComponentChangeTicks<T>>();
        if (!storage)
            return false;
        const auto it = storage->ticks.find(entity);
        return it != storage->ticks.end() && it->second.changed == m_tick;
    }

    template<typename T>
    [[nodiscard]] bool isChangedSince(Entity entity, const entt::registry& registry, uint32_t sinceTick) const
    {
        const auto* storage = registry.ctx().find<ComponentChangeTicks<T>>();
        if (!storage)
            return false;
        const auto it = storage->ticks.find(entity);
        return it != storage->ticks.end() && it->second.changed > sinceTick;
    }

    template<typename T>
    [[nodiscard]] bool anyChangedThisFrame(const entt::registry& registry) const
    {
        return anyMatchingTick<T>(registry, m_tick, &ComponentTicks::changed);
    }

    template<typename T>
    [[nodiscard]] bool anyAddedThisFrame(const entt::registry& registry) const
    {
        return anyMatchingTick<T>(registry, m_tick, &ComponentTicks::added);
    }

    template<typename T>
    [[nodiscard]] bool anyChangedSince(const entt::registry& registry, uint32_t sinceTick) const
    {
        const auto* storage = registry.ctx().find<ComponentChangeTicks<T>>();
        if (!storage)
            return false;
        for (const auto& [entity, componentTicks] : storage->ticks)
        {
            (void)entity;
            if (componentTicks.changed > sinceTick)
                return true;
        }
        return false;
    }

    template<typename... Ts>
    [[nodiscard]] bool anyOfChangedThisFrame(const entt::registry& registry) const
    {
        return (anyChangedThisFrame<Ts>(registry) || ...);
    }

    template<typename... Ts>
    [[nodiscard]] bool anyOfAddedThisFrame(const entt::registry& registry) const
    {
        return (anyAddedThisFrame<Ts>(registry) || ...);
    }

    template<typename... Ts>
    [[nodiscard]] bool anyOfChangedSince(const entt::registry& registry, uint32_t sinceTick) const
    {
        return (anyChangedSince<Ts>(registry, sinceTick) || ...);
    }

private:
    template<typename T>
    static ComponentChangeTicks<T>& storageFor(entt::registry& registry)
    {
        if (auto* storage = registry.ctx().find<ComponentChangeTicks<T>>())
            return *storage;
        return registry.ctx().emplace<ComponentChangeTicks<T>>();
    }

    template<typename T>
    [[nodiscard]] static bool anyMatchingTick(
        const entt::registry& registry,
        uint32_t tick,
        uint32_t ComponentTicks::*field)
    {
        const auto* storage = registry.ctx().find<ComponentChangeTicks<T>>();
        if (!storage)
            return false;
        for (const auto& [entity, componentTicks] : storage->ticks)
        {
            (void)entity;
            if (componentTicks.*field == tick)
                return true;
        }
        return false;
    }

    uint32_t m_tick = 1;
    bool m_worldStructureChanged = false;
};

} // namespace caustica::ecs

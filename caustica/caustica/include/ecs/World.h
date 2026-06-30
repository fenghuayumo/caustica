#pragma once

#include <ecs/ChangeDetection.h>
#include <ecs/Entity.h>
#include <ecs/Query.h>

#include <entt/entity/registry.hpp>

#include <utility>

namespace caustica::ecs
{

class World
{
public:
    Entity spawn()
    {
        return m_registry.create();
    }

    void despawn(Entity entity)
    {
        if (!isAlive(entity))
            return;

        if (auto* changeDetection = getResource<ChangeDetection>())
            changeDetection->noteStructureChange();

        m_registry.destroy(entity);
    }

    [[nodiscard]] bool isAlive(Entity entity) const
    {
        return m_registry.valid(entity);
    }

    void clear()
    {
        m_registry.clear();
        m_registry.ctx().clear();
        m_changeDetectionEnabled = false;
    }

    void enableChangeDetection()
    {
        if (m_changeDetectionEnabled)
            return;

        insertResource<ChangeDetection>();
        m_changeDetectionEnabled = true;
    }

    void endChangeFrame()
    {
        if (auto* changeDetection = getResource<ChangeDetection>())
            changeDetection->endFrame();
    }

    template<typename T>
    void notifyComponentChanged(Entity entity)
    {
        if (auto* changeDetection = getResource<ChangeDetection>())
            changeDetection->markChanged<T>(entity, m_registry);
    }

    template<typename T, typename... Args>
    T& emplace(Entity entity, Args&&... args)
    {
        const bool existed = m_registry.all_of<T>(entity);
        T& result = m_registry.emplace_or_replace<T>(entity, std::forward<Args>(args)...);

        if (auto* changeDetection = getResource<ChangeDetection>())
        {
            if (existed)
                changeDetection->markChanged<T>(entity, m_registry);
            else
                changeDetection->markAdded<T>(entity, m_registry);
        }

        return result;
    }

    template<typename T>
    T* get(Entity entity)
    {
        return m_registry.try_get<T>(entity);
    }

    template<typename T>
    const T* get(Entity entity) const
    {
        return m_registry.try_get<T>(entity);
    }

    template<typename T>
    T* tryGet(Entity entity)
    {
        return m_registry.try_get<T>(entity);
    }

    template<typename T>
    const T* tryGet(Entity entity) const
    {
        return m_registry.try_get<T>(entity);
    }

    template<typename T>
    bool has(Entity entity) const
    {
        return m_registry.all_of<T>(entity);
    }

    template<typename T>
    void remove(Entity entity)
    {
        if (!m_registry.all_of<T>(entity))
            return;

        if (auto* changeDetection = getResource<ChangeDetection>())
            changeDetection->markRemoved<T>(entity, m_registry);

        m_registry.remove<T>(entity);
    }

    template<typename T, typename... Args>
    T& insertResource(Args&&... args)
    {
        return m_registry.ctx().emplace<T>(std::forward<Args>(args)...);
    }

    template<typename T>
    T* getResource()
    {
        return m_registry.ctx().find<T>();
    }

    template<typename T>
    const T* getResource() const
    {
        return m_registry.ctx().find<T>();
    }

    template<typename T, typename Func>
    void each(Func&& func)
    {
        m_registry.view<T>().each([&](Entity entity, T& component) {
            func(entity, component);
        });
    }

    template<typename T, typename Func>
    void each(Func&& func) const
    {
        m_registry.view<T>().each([&](Entity entity, const T& component) {
            func(entity, component);
        });
    }

    template<typename First, typename Second, typename... Rest, typename Func>
    void each(Func&& func)
    {
        using Desc = detail::query_descriptor<First, Second, Rest...>;
        detail::each_query<Desc>(m_registry, getResource<ChangeDetection>(), std::forward<Func>(func));
    }

    template<typename First, typename Second, typename... Rest, typename Func>
    void each(Func&& func) const
    {
        using Desc = detail::query_descriptor<First, Second, Rest...>;
        detail::each_query_const<Desc>(m_registry, getResource<ChangeDetection>(), std::forward<Func>(func));
    }

    [[nodiscard]] entt::registry& registry() { return m_registry; }
    [[nodiscard]] const entt::registry& registry() const { return m_registry; }

private:
    entt::registry m_registry;
    bool m_changeDetectionEnabled = false;
};

} // namespace caustica::ecs

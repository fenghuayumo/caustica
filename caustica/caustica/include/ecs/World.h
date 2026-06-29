#pragma once

#include <ecs/Entity.h>

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
        if (isAlive(entity))
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
    }

    template<typename T, typename... Args>
    T& emplace(Entity entity, Args&&... args)
    {
        return m_registry.emplace_or_replace<T>(entity, std::forward<Args>(args)...);
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
    bool has(Entity entity) const
    {
        return m_registry.all_of<T>(entity);
    }

    template<typename T>
    void remove(Entity entity)
    {
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
        m_registry.view<First, Second, Rest...>().each(
            [&](Entity entity, First& first, Second& second, Rest&... rest) {
                func(entity, first, second, rest...);
            });
    }

    template<typename First, typename Second, typename... Rest, typename Func>
    void each(Func&& func) const
    {
        m_registry.view<First, Second, Rest...>().each(
            [&](Entity entity, const First& first, const Second& second, const Rest&... rest) {
                func(entity, first, second, rest...);
            });
    }

    [[nodiscard]] entt::registry& registry() { return m_registry; }
    [[nodiscard]] const entt::registry& registry() const { return m_registry; }

private:
    entt::registry m_registry;
};

} // namespace caustica::ecs

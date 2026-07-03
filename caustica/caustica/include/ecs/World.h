#pragma once

#include <ecs/ChangeDetection.h>
#include <ecs/Commands.h>
#include <ecs/Entity.h>
#include <ecs/Events.h>
#include <ecs/Query.h>

#include <entt/entity/registry.hpp>

#include <functional>
#include <tuple>
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

    CommandQueue& commands()
    {
        if (auto* queue = getResource<CommandQueue>())
            return *queue;
        return insertResource<CommandQueue>();
    }

    template<typename E>
    Events<E>& events()
    {
        if (auto* buffer = getResource<Events<E>>())
            return *buffer;
        return insertResource<Events<E>>();
    }

    template<typename E>
    EventWriter<E> eventWriter()
    {
        return EventWriter<E>(&events<E>());
    }

    template<typename E>
    EventReader<E> eventReader() const
    {
        return EventReader<E>(getResource<Events<E>>());
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

inline void CommandQueue::despawn(Entity entity)
{
    push([entity](World& world) { world.despawn(entity); });
}

template<typename T>
void CommandQueue::remove(Entity entity)
{
    push([entity](World& world) { world.remove<T>(entity); });
}

inline void CommandQueue::push(std::function<void(World&)> command)
{
    m_commands.push_back(std::move(command));
}

inline void CommandQueue::apply(World& world)
{
    for (std::function<void(World&)>& command : m_commands)
        command(world);
    m_commands.clear();
}

inline void CommandQueue::clear()
{
    m_commands.clear();
}

template<typename T, typename... Args>
void CommandQueue::emplace(Entity entity, Args&&... args)
{
    auto bound = std::make_tuple(std::forward<Args>(args)...);
    push([entity, bound = std::move(bound)](World& world) mutable {
        std::apply([&](auto&&... forwarded) {
            world.emplace<T>(entity, std::forward<decltype(forwarded)>(forwarded)...);
        }, bound);
    });
}

} // namespace caustica::ecs

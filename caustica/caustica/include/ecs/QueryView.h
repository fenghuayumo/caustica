#pragma once

#include <ecs/World.h>

#include <utility>

namespace caustica::ecs
{

// Bevy-style query view over a World (the live App registry after scene commit).
// Injected as a typed system parameter: Query<LocalTransformComponent, MeshInstanceComponent>.
template<typename... Components>
class Query
{
public:
    Query() = default;
    explicit Query(World* world) : m_world(world) {}

    [[nodiscard]] explicit operator bool() const { return m_world != nullptr; }
    [[nodiscard]] World* world() const { return m_world; }

    template<typename Func>
    void each(Func&& func) const
    {
        if (!m_world)
            return;

        if constexpr (sizeof...(Components) == 0)
            static_assert(sizeof...(Components) > 0, "Query<> requires at least one component type");
        else if constexpr (sizeof...(Components) == 1)
            m_world->each<Components...>(std::forward<Func>(func));
        else
            m_world->each<Components...>(std::forward<Func>(func));
    }

private:
    World* m_world = nullptr;
};

} // namespace caustica::ecs

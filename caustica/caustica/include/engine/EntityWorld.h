#pragma once

#include <ecs/Entity.h>
#include <math/math.h>
#include <scene/SceneEcs.h>

#include <optional>
#include <string>
#include <utility>

namespace caustica
{

// Thin system-parameter wrapper around the live scene ECS (not App resource world).
// Prefer this over ctx.entityWorld() / SceneEntityWorld::set* digs in host systems.
class EntityWorld
{
public:
    EntityWorld() = default;
    explicit EntityWorld(scene::SceneEntityWorld* world) : m_world(world) {}

    [[nodiscard]] explicit operator bool() const { return m_world != nullptr; }
    [[nodiscard]] scene::SceneEntityWorld* get() const { return m_world; }
    [[nodiscard]] scene::SceneEntityWorld* operator->() const { return m_world; }
    [[nodiscard]] scene::SceneEntityWorld& operator*() const { return *m_world; }

    [[nodiscard]] ecs::World& ecs() { return m_world->world(); }
    [[nodiscard]] const ecs::World& ecs() const { return m_world->world(); }

    // Hierarchy-aware spawn with optional component bundle (Bevy-style).
    template<typename... Components>
    ecs::Entity spawn(Components&&... components)
    {
        return m_world->spawn(std::forward<Components>(components)...);
    }

    template<typename... Components>
    ecs::Entity spawnNamed(const std::string& name, Components&&... components)
    {
        return m_world->spawnNamed(name, ecs::NullEntity, std::forward<Components>(components)...);
    }

    bool setLocalTransform(
        ecs::Entity entity,
        const std::optional<dm::double3>& translation = std::nullopt,
        const std::optional<dm::dquat>& rotation = std::nullopt,
        const std::optional<dm::double3>& scaling = std::nullopt)
    {
        if (!m_world || !ecs::isValid(entity) || !m_world->world().isAlive(entity))
            return false;

        const dm::double3* t = translation ? &*translation : nullptr;
        const dm::dquat* r = rotation ? &*rotation : nullptr;
        const dm::double3* s = scaling ? &*scaling : nullptr;
        m_world->setLocalTransform(entity, t, r, s);
        return true;
    }

    bool setTranslation(ecs::Entity entity, const dm::double3& translation)
    {
        return setLocalTransform(entity, translation, std::nullopt, std::nullopt);
    }

private:
    scene::SceneEntityWorld* m_world = nullptr;
};

} // namespace caustica

#pragma once

#include <ecs/Entity.h>
#include <math/math.h>
#include <scene/SceneEcs.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace caustica
{

// Thin system-parameter wrapper around the live scene graph (App::world() after commit).
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

    // Tag an existing entity, typically with a marker component so per-frame
    // systems can select it with Query<..., With<Marker>> instead of taking the
    // whole world. Needed for entities this system did not spawn itself, such as
    // prefabs loaded through spawnFromFile.
    template<typename T, typename... Args>
    bool emplace(ecs::Entity entity, Args&&... args)
    {
        if (!m_world || !ecs::isValid(entity) || !m_world->world().isAlive(entity))
            return false;
        m_world->world().emplace<T>(entity, std::forward<Args>(args)...);
        return true;
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

    // MeshInstanceComponent::enabled — no-op (false) when entity has no mesh instance.
    bool setVisible(ecs::Entity entity, bool visible)
    {
        if (!m_world || !ecs::isValid(entity) || !m_world->world().isAlive(entity))
            return false;
        auto* mesh = m_world->world().tryGet<scene::MeshInstanceComponent>(entity);
        if (!mesh)
            return false;
        mesh->enabled = visible;
        return true;
    }

    // Walks from scene root when context is null (App-friendly default).
    [[nodiscard]] ecs::Entity findEntity(
        const std::filesystem::path& path,
        ecs::Entity context = ecs::NullEntity) const
    {
        if (!m_world)
            return ecs::NullEntity;
        if (!ecs::isValid(context))
            context = m_world->root();
        return m_world->findEntity(path, context);
    }

    [[nodiscard]] std::string name(ecs::Entity entity) const
    {
        return m_world ? m_world->getEntityName(entity) : std::string{};
    }

private:
    scene::SceneEntityWorld* m_world = nullptr;
};

} // namespace caustica

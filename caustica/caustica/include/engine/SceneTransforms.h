#pragma once

#include <ecs/Entity.h>
#include <ecs/World.h>
#include <math/math.h>
#include <scene/SceneEcs.h>

#include <optional>

namespace caustica
{

// Parallel-safe transform writer (ADR 0003).
//
// EntityWorld can spawn, despawn and reach any component, so the scheduler has
// to run systems taking it exclusively. This parameter only ever writes
// LocalTransformComponent, so it declares exactly that and overlaps with every
// system that does not touch transforms — which is what lets ordinary
// "move things every frame" systems run in parallel.
//
// It deliberately will not create a transform: an entity without
// LocalTransformComponent is left alone and the call returns false. Adding a
// component is a structural change to the registry and would race systems
// iterating it. Spawn with a transform (or attach one from an exclusive setup
// system) and animate it from here.
class SceneTransforms
{
public:
    SceneTransforms() = default;
    explicit SceneTransforms(ecs::World* world) : m_world(world) {}

    [[nodiscard]] explicit operator bool() const { return m_world != nullptr; }

    bool setLocalTransform(
        ecs::Entity entity,
        const std::optional<math::double3>& translation = std::nullopt,
        const std::optional<math::dquat>& rotation = std::nullopt,
        const std::optional<math::double3>& scaling = std::nullopt)
    {
        if (!m_world || !ecs::isValid(entity) || !m_world->isAlive(entity))
            return false;

        auto* local = m_world->get<scene::LocalTransformComponent>(entity);
        if (!local)
            return false;

        bool changed = !local->hasLocalTransform;
        if (translation && any(*translation != local->translation))
        {
            local->translation = *translation;
            changed = true;
        }
        if (rotation)
        {
            // q and -q are the same orientation; avoid thrashing on sign flips.
            const double align = math::dot(local->rotation, *rotation);
            const math::dquat canonical = (align < 0.0) ? -(*rotation) : *rotation;
            if (any(canonical != local->rotation))
            {
                local->rotation = canonical;
                changed = true;
            }
        }
        if (scaling && any(*scaling != local->scaling))
        {
            local->scaling = *scaling;
            changed = true;
        }

        if (!changed)
            return true;

        local->hasLocalTransform = true;
        local->compose();
        // Safe from a worker: only one system can write a given component type
        // at a time, and the tick storage was created before dispatch.
        m_world->notifyComponentChanged<scene::LocalTransformComponent>(entity);
        return true;
    }

    bool setTranslation(ecs::Entity entity, const math::double3& translation)
    {
        return setLocalTransform(entity, translation, std::nullopt, std::nullopt);
    }

    bool setRotation(ecs::Entity entity, const math::dquat& rotation)
    {
        return setLocalTransform(entity, std::nullopt, rotation, std::nullopt);
    }

    bool setScaling(ecs::Entity entity, const math::double3& scaling)
    {
        return setLocalTransform(entity, std::nullopt, std::nullopt, scaling);
    }

private:
    ecs::World* m_world = nullptr;
};

} // namespace caustica

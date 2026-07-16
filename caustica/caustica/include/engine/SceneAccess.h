#pragma once

#include <ecs/World.h>
#include <scene/Scene.h>

#include <memory>

namespace caustica
{

// App-world resource pointing at the live scene ECS.
// Prefer SystemContext::entityWorld() / caustica::entityWorld(app).
// Do not dig through GpuRenderSubsystem -> SceneManager -> Scene.
struct SceneAccess
{
    std::shared_ptr<Scene> active;

    [[nodiscard]] scene::SceneEntityWorld* entityWorld() const
    {
        return active ? active->getEntityWorld() : nullptr;
    }

    [[nodiscard]] ecs::World* ecs() const
    {
        scene::SceneEntityWorld* ew = entityWorld();
        return ew ? &ew->world() : nullptr;
    }

    [[nodiscard]] bool isValid() const { return entityWorld() != nullptr; }
};

} // namespace caustica

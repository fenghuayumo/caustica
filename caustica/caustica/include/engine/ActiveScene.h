#pragma once

#include <ecs/World.h>
#include <scene/Scene.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace caustica
{

// App-owned committed scene identity. Single source of truth for the live scene
// pointer, name, and path. SceneManager remains the loader/controller.
struct ActiveScene
{
    std::shared_ptr<Scene> scene;
    std::string name;
    std::filesystem::path path;
    uint64_t generation = 0;

    [[nodiscard]] scene::SceneEntityWorld* entityWorld() const
    {
        return scene ? scene->getEntityWorld() : nullptr;
    }

    [[nodiscard]] ecs::World* ecs() const
    {
        scene::SceneEntityWorld* ew = entityWorld();
        return ew ? &ew->world() : nullptr;
    }

    [[nodiscard]] bool isValid() const { return scene != nullptr; }
};

} // namespace caustica

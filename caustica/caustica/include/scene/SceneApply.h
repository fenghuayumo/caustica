#pragma once

#include <ecs/Entity.h>

#include <functional>
#include <memory>

namespace caustica
{

class Scene;
struct Material;
struct SceneImportResult;

// Logic-thread scene graph edits (Bevy-style: change the world, do not touch GPU).
// Structure edits mark Scene::requestGpuStructureSync(); Extract flushes mesh upload
// and AS rebuild before publishing UE-style render proxies.

struct SceneApplyCallbacks
{
    std::function<void(Material&)> postMaterialLoad;
};

struct DestroySceneEntityParams
{
    std::shared_ptr<Scene> scene;
    ecs::Entity entity = ecs::NullEntity;
    // Optional hook before ECS detach (e.g. drop render-owned splat objects).
    std::function<void(ecs::Entity)> beforeDetach;
};

// Grafts an importer subtree into the live ECS. Does not upload GPU or publish proxies.
ecs::Entity attachImportedScene(
    const std::shared_ptr<Scene>& scene,
    const SceneImportResult& importResult,
    const SceneApplyCallbacks& callbacks = {});

void applyImportedSceneMaterialCallbacks(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity importedRoot,
    const SceneApplyCallbacks& callbacks);

bool destroySceneEntity(const DestroySceneEntityParams& params);

} // namespace caustica

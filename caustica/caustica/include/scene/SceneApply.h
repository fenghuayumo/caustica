#pragma once

#include <ecs/Entity.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace caustica
{

class Scene;
struct Material;
struct SceneImportResult;

// Logic-thread scene graph edits (Bevy-style: change the world, do not touch GPU).
// After attach/destroy, call sceneSession::syncSceneGpu so the render thread
// uploads resources and publishes UE-style render proxies.

struct SceneApplyCallbacks
{
    std::function<void(Material&)> postMaterialLoad;
};

struct DestroySceneEntityParams
{
    std::shared_ptr<Scene> scene;
    ecs::Entity entity = ecs::NullEntity;
    uint32_t frameIndex = 0;
    // Caller must drain the render thread / GPU before destroy.
    std::function<void(ecs::Entity)> beforeDetach;
};

// Grafts an importer subtree into the live ECS. Does not publish render proxies.
ecs::Entity attachImportedScene(
    const std::shared_ptr<Scene>& scene,
    const SceneImportResult& importResult,
    const SceneApplyCallbacks& callbacks = {});

void applyImportedSceneMaterialCallbacks(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity importedRoot,
    const SceneApplyCallbacks& callbacks);

// Publishes a render snapshot only. Prefer syncSceneGpu so mesh GPU buffers
// exist before proxies are visible to the render thread.
void publishSceneRenderProxies(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity importedRoot,
    uint32_t frameIndex,
    const SceneApplyCallbacks& callbacks = {});

bool destroySceneEntity(const DestroySceneEntityParams& params);

} // namespace caustica

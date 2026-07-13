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

namespace scene { class SceneEntityWorld; }

struct RuntimeSceneMutationCallbacks
{
    std::function<void(Material&)> PostMaterialLoad;
};

struct DeleteRuntimeSceneNodeParams
{
    std::shared_ptr<Scene> SceneInstance;
    ecs::Entity Entity = ecs::NullEntity;
    uint32_t FrameIndex = 0;
    // Caller must drain render thread / GPU before invoking delete.
    std::function<void(ecs::Entity)> BeforeDetach;
};

ecs::Entity AttachRuntimeSceneImport(
    const std::shared_ptr<Scene>& scene,
    const SceneImportResult& importResult,
    uint32_t frameIndex,
    const RuntimeSceneMutationCallbacks& callbacks = {});

void FinalizeRuntimeSceneMutation(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity importedRoot,
    uint32_t frameIndex,
    const RuntimeSceneMutationCallbacks& callbacks = {});

bool DeleteRuntimeSceneNode(const DeleteRuntimeSceneNodeParams& params);

} // namespace caustica

#pragma once

// Compatibility aliases for older call sites. Prefer SceneApply.h.
#include <scene/SceneApply.h>

namespace caustica
{

using RuntimeSceneMutationCallbacks = SceneApplyCallbacks;
using DeleteRuntimeSceneNodeParams = DestroySceneEntityParams;

inline ecs::Entity attachRuntimeSceneImport(
    const std::shared_ptr<Scene>& scene,
    const SceneImportResult& importResult,
    const RuntimeSceneMutationCallbacks& callbacks = {})
{
    return attachImportedScene(scene, importResult, callbacks);
}

inline void finalizeRuntimeSceneMutation(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity importedRoot,
    uint32_t frameIndex,
    const RuntimeSceneMutationCallbacks& callbacks = {})
{
    publishSceneRenderProxies(scene, importedRoot, frameIndex, callbacks);
}

inline void applyRuntimeSceneMaterialCallbacks(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity importedRoot,
    const RuntimeSceneMutationCallbacks& callbacks)
{
    applyImportedSceneMaterialCallbacks(scene, importedRoot, callbacks);
}

inline bool deleteRuntimeSceneNode(const DeleteRuntimeSceneNodeParams& params)
{
    return destroySceneEntity(params);
}

} // namespace caustica

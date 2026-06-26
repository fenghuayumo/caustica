#pragma once

#include <rhi/nvrhi.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace caustica
{

class Scene;
class SceneGraphNode;
struct Material;
struct SceneImportResult;

struct RuntimeSceneMutationCallbacks
{
    std::function<void(Material&)> PostMaterialLoad;
};

struct DeleteRuntimeSceneNodeParams
{
    std::shared_ptr<Scene> SceneInstance;
    std::shared_ptr<SceneGraphNode> Node;
    nvrhi::IDevice* Device = nullptr;
    uint32_t FrameIndex = 0;
    std::function<void(const std::shared_ptr<SceneGraphNode>&)> BeforeDetach;
};

std::shared_ptr<SceneGraphNode> AttachRuntimeSceneImport(
    const std::shared_ptr<Scene>& scene,
    const SceneImportResult& importResult,
    uint32_t frameIndex,
    const RuntimeSceneMutationCallbacks& callbacks = {});

void FinalizeRuntimeSceneMutation(
    const std::shared_ptr<Scene>& scene,
    const std::shared_ptr<SceneGraphNode>& importedRoot,
    uint32_t frameIndex,
    const RuntimeSceneMutationCallbacks& callbacks = {});

bool DeleteRuntimeSceneNode(const DeleteRuntimeSceneNodeParams& params);

} // namespace caustica

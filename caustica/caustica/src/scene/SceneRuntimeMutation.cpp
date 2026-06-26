#include <scene/SceneRuntimeMutation.h>

#include <scene/Scene.h>
#include <scene/SceneGraph.h>

#include <unordered_set>

namespace caustica
{

std::shared_ptr<SceneGraphNode> AttachRuntimeSceneImport(
    const std::shared_ptr<Scene>& scene,
    const SceneImportResult& importResult,
    uint32_t frameIndex,
    const RuntimeSceneMutationCallbacks& callbacks)
{
    if (!scene || !scene->GetSceneGraph() || !importResult.rootNode)
        return nullptr;

    auto sceneGraph = scene->GetSceneGraph();
    auto importedRoot = sceneGraph->Attach(sceneGraph->GetRootNode(), importResult.rootNode);
    FinalizeRuntimeSceneMutation(scene, importedRoot, frameIndex, callbacks);
    return importedRoot;
}

void FinalizeRuntimeSceneMutation(
    const std::shared_ptr<Scene>& scene,
    const std::shared_ptr<SceneGraphNode>& importedRoot,
    uint32_t frameIndex,
    const RuntimeSceneMutationCallbacks& callbacks)
{
    if (importedRoot && callbacks.PostMaterialLoad)
    {
        std::unordered_set<caustica::Material*> processedMaterials;
        SceneGraphWalker walker(importedRoot.get());
        while (walker)
        {
            auto meshInstance = std::dynamic_pointer_cast<MeshInstance>(walker->GetLeaf());
            if (meshInstance && meshInstance->GetMesh())
            {
                for (const auto& geometry : meshInstance->GetMesh()->geometries)
                {
                    if (geometry->material && processedMaterials.insert(geometry->material.get()).second)
                        callbacks.PostMaterialLoad(*geometry->material);
                }
            }
            walker.Next(true);
        }
    }

    if (scene)
        scene->FinishedLoading(frameIndex);
}

bool DeleteRuntimeSceneNode(const DeleteRuntimeSceneNodeParams& params)
{
    if (params.Node == nullptr || params.SceneInstance == nullptr)
        return false;

    auto sceneGraph = params.SceneInstance->GetSceneGraph();
    if (sceneGraph == nullptr)
        return false;

    auto rootNode = sceneGraph->GetRootNode();
    if (rootNode == nullptr || params.Node == rootNode)
        return false;

    if (params.Node->GetGraph() != sceneGraph || params.Node->GetParent() == nullptr)
        return false;

    if (params.Device)
        params.Device->waitForIdle();

    if (params.BeforeDetach)
        params.BeforeDetach(params.Node);

    sceneGraph->Detach(params.Node, true);
    params.SceneInstance->FinishedLoading(params.FrameIndex);
    return true;
}

} // namespace caustica

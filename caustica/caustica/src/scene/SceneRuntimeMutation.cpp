#include <scene/SceneRuntimeMutation.h>

#include <scene/Scene.h>
#include <scene/SceneImport.h>
#include <scene/SceneObjects.h>

#include <unordered_set>

namespace caustica
{

namespace
{
void ForEachEntityInSubtree(scene::SceneEntityWorld& world, ecs::Entity root, const auto& fn)
{
    if (!ecs::isValid(root))
        return;

    fn(root);

    for (ecs::Entity child : world.getEntityChildren(root))
        ForEachEntityInSubtree(world, child, fn);
}
} // namespace

ecs::Entity attachRuntimeSceneImport(
    const std::shared_ptr<Scene>& scene,
    const SceneImportResult& importResult,
    uint32_t frameIndex,
    const RuntimeSceneMutationCallbacks& callbacks)
{
    if (!scene || !scene->getEntityWorld() || !importResult.entityWorld || !ecs::isValid(importResult.rootEntity))
        return ecs::NullEntity;

    auto* entityWorld = scene->getEntityWorld();
    ecs::Entity importedRoot = entityWorld->importSubtree(
        entityWorld->root(),
        *importResult.entityWorld,
        importResult.rootEntity,
        scene->getSceneTypeFactory().get());

    finalizeRuntimeSceneMutation(scene, importedRoot, frameIndex, callbacks);
    return importedRoot;
}

void finalizeRuntimeSceneMutation(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity importedRoot,
    uint32_t frameIndex,
    const RuntimeSceneMutationCallbacks& callbacks)
{
    if (ecs::isValid(importedRoot) && callbacks.PostMaterialLoad && scene && scene->getEntityWorld())
    {
        std::unordered_set<Material*> processedMaterials;
        ForEachEntityInSubtree(*scene->getEntityWorld(), importedRoot, [&](ecs::Entity entity) {
            auto* meshComp = scene->getEntityWorld()->world().get<scene::MeshInstanceComponent>(entity);
            if (!meshComp || !meshComp->mesh)
                return;

            for (const auto& geometry : meshComp->mesh->geometries)
            {
                if (geometry->material && processedMaterials.insert(geometry->material.get()).second)
                    callbacks.PostMaterialLoad(*geometry->material);
            }
        });
    }

    if (scene)
        scene->extractAndPublishRenderSnapshot(frameIndex);
}

bool deleteRuntimeSceneNode(const DeleteRuntimeSceneNodeParams& params)
{
    if (!ecs::isValid(params.Entity) || params.SceneInstance == nullptr)
        return false;

    auto* entityWorld = params.SceneInstance->getEntityWorld();
    if (!entityWorld || !ecs::isValid(entityWorld->root()))
        return false;

    if (params.Entity == entityWorld->root())
        return false;

    if (!entityWorld->world().isAlive(params.Entity))
        return false;

    if (params.BeforeDetach)
        params.BeforeDetach(params.Entity);

    entityWorld->destroyEntity(params.Entity);
    entityWorld->rebuildPathsFromRoot();
    // Reindex + publish in one path (same as import finalize). Caller must drain
    // render thread / GPU first so snapshot readers are idle.
    params.SceneInstance->extractAndPublishRenderSnapshot(params.FrameIndex);
    return true;
}

} // namespace caustica

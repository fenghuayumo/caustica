#include <scene/SceneApply.h>

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

void ApplyMaterialCallbacks(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity importedRoot,
    const SceneApplyCallbacks& callbacks)
{
    if (!ecs::isValid(importedRoot) || !callbacks.postMaterialLoad || !scene || !scene->getEntityWorld())
        return;

    std::unordered_set<Material*> processedMaterials;
    ForEachEntityInSubtree(*scene->getEntityWorld(), importedRoot, [&](ecs::Entity entity) {
        auto* meshComp = scene->getEntityWorld()->world().get<scene::MeshInstanceComponent>(entity);
        if (!meshComp || !meshComp->mesh)
            return;

        for (const auto& geometry : meshComp->mesh->geometries)
        {
            if (geometry->material && processedMaterials.insert(geometry->material.get()).second)
                callbacks.postMaterialLoad(*geometry->material);
        }
    });
}
} // namespace

ecs::Entity attachImportedScene(
    const std::shared_ptr<Scene>& scene,
    const SceneImportResult& importResult,
    const SceneApplyCallbacks& callbacks)
{
    if (!scene || !scene->getEntityWorld() || !importResult.entityWorld || !ecs::isValid(importResult.rootEntity))
        return ecs::NullEntity;

    auto* entityWorld = scene->getEntityWorld();
    ecs::Entity importedRoot = entityWorld->importSubtree(
        entityWorld->root(),
        *importResult.entityWorld,
        importResult.rootEntity,
        scene->getSceneTypeFactory().get());

    ApplyMaterialCallbacks(scene, importedRoot, callbacks);
    return importedRoot;
}

void applyImportedSceneMaterialCallbacks(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity importedRoot,
    const SceneApplyCallbacks& callbacks)
{
    ApplyMaterialCallbacks(scene, importedRoot, callbacks);
}

void publishSceneRenderProxies(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity importedRoot,
    uint32_t frameIndex,
    const SceneApplyCallbacks& callbacks)
{
    ApplyMaterialCallbacks(scene, importedRoot, callbacks);

    if (scene)
        scene->extractAndPublishRenderSnapshot(frameIndex);
}

bool destroySceneEntity(const DestroySceneEntityParams& params)
{
    if (!ecs::isValid(params.entity) || params.scene == nullptr)
        return false;

    auto* entityWorld = params.scene->getEntityWorld();
    if (!entityWorld || !ecs::isValid(entityWorld->root()))
        return false;

    if (params.entity == entityWorld->root())
        return false;

    if (!entityWorld->world().isAlive(params.entity))
        return false;

    if (params.beforeDetach)
        params.beforeDetach(params.entity);

    entityWorld->destroyEntity(params.entity);
    entityWorld->rebuildPathsFromRoot();
    params.scene->extractAndPublishRenderSnapshot(params.frameIndex);
    return true;
}

} // namespace caustica

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

bool WouldRemoveLastEnvironmentLight(scene::SceneEntityWorld& entityWorld, ecs::Entity subtree)
{
    size_t environmentLightCount = 0;
    size_t removedEnvironmentLightCount = 0;
    entityWorld.world().each<scene::EnvironmentLightComponent>(
        [&](ecs::Entity light, scene::EnvironmentLightComponent&) {
            ++environmentLightCount;
            if (entityWorld.entitySubtreeContains(subtree, light))
                ++removedEnvironmentLightCount;
        });
    return environmentLightCount > 0 && removedEnvironmentLightCount == environmentLightCount;
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
    if (ecs::isValid(importedRoot))
        scene->requestGpuStructureSync();
    return importedRoot;
}

void applyImportedSceneMaterialCallbacks(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity importedRoot,
    const SceneApplyCallbacks& callbacks)
{
    ApplyMaterialCallbacks(scene, importedRoot, callbacks);
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

    // An active scene must retain at least one environment-light entity.
    // Keep this invariant in the engine path as well as the editor UI so
    // scripting and future clients cannot bypass it.
    if (WouldRemoveLastEnvironmentLight(*entityWorld, params.entity))
        return false;

    const auto* content = entityWorld->world().get<scene::SceneContentComponent>(params.entity);
    const SceneContentFlags subtree = content ? content->subgraphContent : SceneContentFlags::None;
    const bool containsLights = (subtree & SceneContentFlags::Lights) != 0;
    const bool lightOnly = containsLights && (subtree & ~SceneContentFlags::Lights) == 0;

    if (params.beforeDetach)
        params.beforeDetach(params.entity);

    entityWorld->destroyEntity(params.entity);
    entityWorld->rebuildPathsFromRoot();
    if (!lightOnly)
        params.scene->requestGpuStructureSync();
    return true;
}

} // namespace caustica

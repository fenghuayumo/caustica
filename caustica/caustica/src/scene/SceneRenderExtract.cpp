#include <scene/SceneRenderExtract.h>
#include <scene/SceneRenderData.h>
#include <scene/SceneEcs.h>

#include <algorithm>
#include <vector>

namespace caustica::scene
{

void SceneRenderData::clear()
{
    meshInstances.clear();
    skinnedMeshes.clear();
    meshInstanceEntities.clear();
    skinnedMeshInstanceEntities.clear();
    lightEntities.clear();
    cameraEntities.clear();
    animationEntities.clear();
}

void ExtractSceneRenderData(const SceneEntityWorld& entityWorld, SceneRenderData& out)
{
    out.clear();

    const ecs::World& world = entityWorld.world();

    // Must match SceneEntityWorld::refreshInstanceIndices ordering (stable by entity id).
    struct MeshInstanceRef
    {
        ecs::Entity entity = ecs::NullEntity;
        MeshInstanceComponent* meshComp = nullptr;
        GlobalTransformComponent* global = nullptr;
        BoundsComponent* bounds = nullptr;
        SceneContentComponent* content = nullptr;
    };
    std::vector<MeshInstanceRef> meshRefs;
    world.each<MeshInstanceComponent, GlobalTransformComponent, BoundsComponent, SceneContentComponent>(
        [&](ecs::Entity entity, MeshInstanceComponent& meshComp, GlobalTransformComponent& global,
            BoundsComponent& bounds, SceneContentComponent& content)
        {
            meshRefs.push_back(MeshInstanceRef{ entity, &meshComp, &global, &bounds, &content });
        });
    std::sort(meshRefs.begin(), meshRefs.end(), [](const MeshInstanceRef& a, const MeshInstanceRef& b) {
        return static_cast<uint32_t>(a.entity) < static_cast<uint32_t>(b.entity);
    });

    for (const MeshInstanceRef& ref : meshRefs)
    {
        MeshInstanceRenderProxy proxy;
        proxy.entity = ref.entity;
        proxy.instanceIndex = ref.meshComp->instanceIndex;
        proxy.geometryInstanceIndex = ref.meshComp->geometryInstanceIndex;
        proxy.meshShared = ref.meshComp->mesh;
        proxy.mesh = ref.meshComp->mesh.get();
        proxy.transformFloat = ref.global->transformFloat;
        proxy.globalBounds = ref.bounds->globalBounds;
        proxy.leafContent = ref.content->leafContent;
        out.meshInstances.push_back(std::move(proxy));
        out.meshInstanceEntities.push_back(ref.entity);
    }

    world.each<SkinnedMeshComponent, MeshInstanceComponent>(
        [&](ecs::Entity entity, SkinnedMeshComponent& skinned, MeshInstanceComponent& meshInstance)
        {
            out.skinnedMeshes.push_back(SkinnedMeshRenderProxy{
                entity,
                &skinned,
                &meshInstance,
            });
            out.skinnedMeshInstanceEntities.push_back(entity);
        });

    world.each<LightComponent>([&](ecs::Entity entity, const LightComponent&) {
        out.lightEntities.push_back(entity);
    });

    for (ecs::Entity entity : entityWorld.cameraEntitiesInRegistrationOrder())
    {
        if (world.isAlive(entity) && world.has<CameraComponent>(entity))
            out.cameraEntities.push_back(entity);
    }

    world.each<AnimationComponent>([&](ecs::Entity entity, const AnimationComponent&) {
        out.animationEntities.push_back(entity);
    });
}

} // namespace caustica::scene

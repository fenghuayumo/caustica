#include <scene/SceneRenderExtract.h>
#include <scene/SceneRenderData.h>
#include <scene/SceneEcs.h>

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

    world.each<MeshInstanceComponent, GlobalTransformComponent, BoundsComponent, SceneContentComponent>(
        [&](ecs::Entity entity, MeshInstanceComponent& meshComp, GlobalTransformComponent& global,
            BoundsComponent& bounds, SceneContentComponent& content)
        {
            MeshInstanceRenderProxy proxy;
            proxy.entity = entity;
            proxy.instanceIndex = meshComp.instanceIndex;
            proxy.geometryInstanceIndex = meshComp.geometryInstanceIndex;
            proxy.meshShared = meshComp.mesh;
            proxy.mesh = meshComp.mesh.get();
            proxy.transformFloat = global.transformFloat;
            proxy.globalBounds = bounds.globalBounds;
            proxy.leafContent = content.leafContent;
            out.meshInstances.push_back(std::move(proxy));
            out.meshInstanceEntities.push_back(entity);
        });

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

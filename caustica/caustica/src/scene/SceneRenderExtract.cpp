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

void extractSceneRenderData(SceneEntityWorld& entityWorld, SceneRenderData& out, uint32_t frameIndex)
{
    out.clear();

    ecs::World& world = entityWorld.world();

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
        proxy.previousTransformFloat = ref.global->previousTransformFloat;
        proxy.globalBounds = ref.bounds->globalBounds;
        proxy.leafContent = ref.content->leafContent;
        out.meshInstances.push_back(std::move(proxy));
        out.meshInstanceEntities.push_back(ref.entity);
    }

    world.each<SkinnedMeshComponent, MeshInstanceComponent, GlobalTransformComponent>(
        [&](ecs::Entity entity, SkinnedMeshComponent& skinned, MeshInstanceComponent& meshInstance,
            GlobalTransformComponent& ownerGlobal)
        {
            SkinnedMeshRenderProxy proxy;
            proxy.entity = entity;
            proxy.mesh = meshInstance.mesh;
            proxy.prototypeMesh = skinned.prototypeMesh;
            proxy.transformFloat = ownerGlobal.transformFloat;
            if (const auto* name = world.get<NameComponent>(entity))
                proxy.debugName = name->value;

            const bool forceUpdate = skinned.lastUpdateFrameIndex == kForceSkinnedMeshUpdateFrameIndex;
            proxy.needsSkinningUpdate =
                forceUpdate || skinned.lastUpdateFrameIndex + 1 >= frameIndex;

            // clear FORCE on the logic thread so render never writes back into ECS.
            if (forceUpdate)
                skinned.lastUpdateFrameIndex = frameIndex;

            const dm::daffine3 worldToRoot = inverse(ownerGlobal.transform);
            proxy.jointMatrices.resize(skinned.joints.size());
            proxy.jointLines.resize(skinned.joints.size());

            for (size_t i = 0; i < skinned.joints.size(); ++i)
            {
                const SkinnedMeshJoint& joint = skinned.joints[i];
                const auto* jointGlobal = world.get<GlobalTransformComponent>(joint.jointEntity);
                if (!jointGlobal)
                {
                    proxy.jointMatrices[i] = dm::float4x4::identity();
                    continue;
                }

                const dm::float4x4 jointLocalToRoot =
                    dm::affineToHomogeneous(dm::affine3(jointGlobal->transform * worldToRoot));
                proxy.jointMatrices[i] = joint.inverseBindMatrix * jointLocalToRoot;

                SkinnedMeshJointLineProxy& line = proxy.jointLines[i];
                line.jointPosition = (dm::float4(0.f, 0.f, 0.f, 1.f) * jointLocalToRoot).xyz();
                line.hasParent = false;

                ecs::Entity parentEntity = ecs::NullEntity;
                if (const auto* parent = world.get<ParentComponent>(joint.jointEntity))
                    parentEntity = parent->parent;

                if (ecs::isValid(parentEntity))
                {
                    if (const auto* parentGlobal = world.get<GlobalTransformComponent>(parentEntity))
                    {
                        const dm::float4x4 parentLocalToRoot =
                            dm::affineToHomogeneous(dm::affine3(parentGlobal->transform * worldToRoot));
                        line.parentPosition = (dm::float4(0.f, 0.f, 0.f, 1.f) * parentLocalToRoot).xyz();
                        line.hasParent = true;
                    }
                }

                const dm::float4x4 instanceTransform = dm::affineToHomogeneous(proxy.transformFloat);
                line.jointPosition = (dm::float4(line.jointPosition, 1.f) * instanceTransform).xyz();
                if (line.hasParent)
                    line.parentPosition = (dm::float4(line.parentPosition, 1.f) * instanceTransform).xyz();
            }

            out.skinnedMeshes.push_back(std::move(proxy));
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

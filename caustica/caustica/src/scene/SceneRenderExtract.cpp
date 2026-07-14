#include <scene/SceneRenderExtract.h>
#include <scene/SceneRenderData.h>
#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>

#include <algorithm>
#include <vector>

namespace caustica::scene
{

void SceneRenderData::clear()
{
    meshInstances.clear();
    skinnedMeshes.clear();
    lights.clear();
    camera = {};
    renderSettings = {};
    meshInstanceEntities.clear();
    skinnedMeshInstanceEntities.clear();
    lightEntities.clear();
    cameraEntities.clear();
    animationEntities.clear();
}

const LightRenderProxy* SceneRenderData::findLight(ecs::Entity entity) const
{
    if (!ecs::isValid(entity))
        return nullptr;
    for (const LightRenderProxy& light : lights)
    {
        if (light.entity == entity)
            return &light;
    }
    return nullptr;
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
        proxy.proxiedAnalyticLight = ref.meshComp->proxiedAnalyticLight;

        if (const auto* parent = world.get<ParentComponent>(ref.entity);
            parent && ecs::isValid(parent->parent) && hasAnyLightComponent(world, parent->parent))
        {
            proxy.parentLightEntity = parent->parent;
        }

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

    auto extractLight = [&](ecs::Entity entity, dm::float3 color, const std::vector<std::string>& proxies,
                            LightData data, const GlobalTransformComponent& global)
    {
        LightRenderProxy proxy;
        proxy.entity = entity;
        proxy.color = color;
        proxy.proxies = proxies;
        proxy.data = std::move(data);
        proxy.transform = global.transform;
        out.lights.push_back(std::move(proxy));
        out.lightEntities.push_back(entity);
    };

    world.each<DirectionalLightComponent, GlobalTransformComponent>(
        [&](ecs::Entity entity, const DirectionalLightComponent& light, const GlobalTransformComponent& global)
        {
            extractLight(entity, light.color, {}, toLightData(light), global);
        });
    world.each<SpotLightComponent, GlobalTransformComponent>(
        [&](ecs::Entity entity, const SpotLightComponent& light, const GlobalTransformComponent& global)
        {
            extractLight(entity, light.color, light.proxies, toLightData(light), global);
        });
    world.each<PointLightComponent, GlobalTransformComponent>(
        [&](ecs::Entity entity, const PointLightComponent& light, const GlobalTransformComponent& global)
        {
            extractLight(entity, light.color, light.proxies, toLightData(light), global);
        });
    world.each<EnvironmentLightComponent, GlobalTransformComponent>(
        [&](ecs::Entity entity, const EnvironmentLightComponent& light, const GlobalTransformComponent& global)
        {
            extractLight(entity, light.color, {}, toLightData(light), global);
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

#include <render/core/CameraController.h>

namespace caustica::scene
{

void extractSessionRenderState(const SessionRenderExtractInputs& inputs, SceneRenderData& out)
{
    if (inputs.camera)
    {
        const CameraController& camera = *inputs.camera;
        out.camera.position = camera.camera().getPosition();
        out.camera.direction = camera.camera().getDir();
        out.camera.up = camera.camera().getUp();
        out.camera.verticalFovDegrees = camera.verticalFOV();
        out.camera.zNear = camera.zNear();
        out.camera.useCustomIntrinsics = camera.useCustomIntrinsics();
        out.camera.intrinsics = camera.intrinsics();
        out.camera.intrinsicsViewport = camera.intrinsicsViewport();
        out.camera.selectedCameraIndex = camera.selectedCameraIndex();
    }

    if (inputs.settings)
    {
        out.renderSettings.settings = *inputs.settings;
        // One-shots are owned by this frame's snapshot; clear live so GT does not re-fire.
        inputs.settings->ResetAccumulation = false;
        inputs.settings->ResetRealtimeCaches = false;
        inputs.settings->NRDModeChanged = false;
    }

    if (inputs.runtime)
    {
        out.renderSettings.invalidation = inputs.runtime->Invalidation;
        out.renderSettings.picking = inputs.runtime->Picking;
        // Consume delayed reload request into snapshot only (keep live countdown on GT).
    }

    out.renderSettings.gaussianSplatTemporalReset = inputs.gaussianSplatTemporalReset;
    out.renderSettings.sceneTime = inputs.sceneTime;
}

} // namespace caustica::scene

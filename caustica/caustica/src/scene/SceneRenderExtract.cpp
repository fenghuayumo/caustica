#include <scene/SceneRenderExtract.h>
#include <scene/SceneRenderData.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>

#include <render/core/CameraController.h>
#include <render/core/PathTracerSettings.h>
#include <render/core/ToneMappingParameters.h>
#include <render/SceneGaussianSplatPasses.h>

#include <algorithm>
#include <vector>

namespace caustica::scene
{

void SceneRenderData::clear()
{
    meshInstances.clear();
    skinnedMeshes.clear();
    lights.clear();
    cameras.clear();
    gaussianSplats.clear();
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

const CameraRenderProxy* SceneRenderData::findCamera(ecs::Entity entity) const
{
    if (!ecs::isValid(entity))
        return nullptr;
    for (const CameraRenderProxy& cameraProxy : cameras)
    {
        if (cameraProxy.entity == entity)
            return &cameraProxy;
    }
    return nullptr;
}

namespace
{

struct MeshInstanceRef
{
    ecs::Entity entity = ecs::NullEntity;
    MeshInstanceComponent* meshComp = nullptr;
    GlobalTransformComponent* global = nullptr;
    BoundsComponent* bounds = nullptr;
    SceneContentComponent* content = nullptr;
};

void CollectMeshInstanceRefs(ecs::World& world, std::vector<MeshInstanceRef>& meshRefs)
{
    meshRefs.clear();
    world.each<MeshInstanceComponent, GlobalTransformComponent, BoundsComponent, SceneContentComponent>(
        [&](ecs::Entity entity, MeshInstanceComponent& meshComp, GlobalTransformComponent& global,
            BoundsComponent& bounds, SceneContentComponent& content)
        {
            meshRefs.push_back(MeshInstanceRef{ entity, &meshComp, &global, &bounds, &content });
        });
    std::sort(meshRefs.begin(), meshRefs.end(), [](const MeshInstanceRef& a, const MeshInstanceRef& b) {
        return static_cast<uint32_t>(a.entity) < static_cast<uint32_t>(b.entity);
    });
}

void FillMeshInstanceProxy(
    ecs::World& world,
    const MeshInstanceRef& ref,
    MeshInstanceRenderProxy& proxy)
{
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
    proxy.parentLightEntity = ecs::NullEntity;

    if (const auto* parent = world.get<ParentComponent>(ref.entity);
        parent && ecs::isValid(parent->parent) && hasAnyLightComponent(world, parent->parent))
    {
        proxy.parentLightEntity = parent->parent;
    }
}

void ExtractMeshInstancesFull(ecs::World& world, SceneRenderData& out)
{
    out.meshInstances.clear();
    out.meshInstanceEntities.clear();

    std::vector<MeshInstanceRef> meshRefs;
    CollectMeshInstanceRefs(world, meshRefs);

    out.meshInstances.reserve(meshRefs.size());
    out.meshInstanceEntities.reserve(meshRefs.size());
    for (const MeshInstanceRef& ref : meshRefs)
    {
        MeshInstanceRenderProxy proxy;
        FillMeshInstanceProxy(world, ref, proxy);
        out.meshInstanceEntities.push_back(ref.entity);
        out.meshInstances.push_back(std::move(proxy));
    }
}

bool ExtractMeshInstancesTransforms(ecs::World& world, SceneRenderData& inout)
{
    std::vector<MeshInstanceRef> meshRefs;
    CollectMeshInstanceRefs(world, meshRefs);
    if (meshRefs.size() != inout.meshInstances.size())
        return false;

    for (size_t i = 0; i < meshRefs.size(); ++i)
    {
        if (meshRefs[i].entity != inout.meshInstances[i].entity)
            return false;
        FillMeshInstanceProxy(world, meshRefs[i], inout.meshInstances[i]);
        inout.meshInstanceEntities[i] = meshRefs[i].entity;
    }
    return true;
}

void ExtractSkinnedMeshes(ecs::World& world, SceneRenderData& out, uint32_t frameIndex)
{
    out.skinnedMeshes.clear();
    out.skinnedMeshInstanceEntities.clear();

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
}

void ExtractLightsFull(ecs::World& world, SceneRenderData& out)
{
    out.lights.clear();
    out.lightEntities.clear();

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
}

bool ExtractLightsTransforms(ecs::World& world, SceneRenderData& inout)
{
    // Lights are few; rebuild when the set drifts, otherwise patch transforms/color/data.
    SceneRenderData rebuilt;
    ExtractLightsFull(world, rebuilt);
    if (rebuilt.lights.size() != inout.lights.size())
    {
        inout.lights = std::move(rebuilt.lights);
        inout.lightEntities = std::move(rebuilt.lightEntities);
        return true;
    }

    for (size_t i = 0; i < rebuilt.lights.size(); ++i)
    {
        if (rebuilt.lights[i].entity != inout.lights[i].entity)
        {
            inout.lights = std::move(rebuilt.lights);
            inout.lightEntities = std::move(rebuilt.lightEntities);
            return true;
        }
        inout.lights[i] = std::move(rebuilt.lights[i]);
        inout.lightEntities[i] = rebuilt.lightEntities[i];
    }
    return true;
}

void ExtractCameras(SceneEntityWorld& entityWorld, ecs::World& world, SceneRenderData& out)
{
    out.cameras.clear();
    out.cameraEntities.clear();

    for (ecs::Entity entity : entityWorld.cameraEntitiesInRegistrationOrder())
    {
        if (!world.isAlive(entity))
            continue;
        const CameraComponent* camComp = tryGetCamera(world, entity);
        const GlobalTransformComponent* global = world.get<GlobalTransformComponent>(entity);
        if (!camComp || !global)
            continue;

        out.cameraEntities.push_back(entity);
        out.cameras.push_back(makeCameraRenderProxy(entity, *camComp, *global));
    }
}

void ExtractGaussianSplats(ecs::World& world, SceneRenderData& out)
{
    out.gaussianSplats.clear();
    world.each<GaussianSplatComponent, GlobalTransformComponent>(
        [&](ecs::Entity entity, const GaussianSplatComponent& splat, const GlobalTransformComponent& global)
        {
            GaussianSplatRenderProxy proxy;
            proxy.entity = entity;
            proxy.enabled = splat.splat.enabled;
            proxy.objectToWorld = global.transformFloat;
            out.gaussianSplats.push_back(std::move(proxy));
        });
}

void ExtractAnimationEntities(ecs::World& world, SceneRenderData& out)
{
    out.animationEntities.clear();
    world.each<AnimationComponent>([&](ecs::Entity entity, const AnimationComponent&) {
        out.animationEntities.push_back(entity);
    });
}

void FillActiveCameraFromFreeController(const CameraController& camera, ActiveCameraRenderProxy& out)
{
    out.sourceEntity = ecs::NullEntity;
    out.selectedCameraIndex = camera.selectedCameraIndex();
    out.position = camera.camera().getPosition();
    out.direction = camera.camera().getDir();
    out.up = camera.camera().getUp();
    out.verticalFovRadians = camera.verticalFOV();
    out.zNear = camera.zNear();
    out.useCustomIntrinsics = camera.useCustomIntrinsics();
    out.intrinsics = camera.intrinsics();
    out.intrinsicsViewport = camera.intrinsicsViewport();
    out.valid = true;
}

void FillActiveCameraFromPerspectiveProxy(const CameraRenderProxy& proxy, uint32_t selectedIndex, ActiveCameraRenderProxy& out)
{
    const dm::affine3 viewToWorld = getCameraViewToWorldMatrix(proxy.transform);
    out.sourceEntity = proxy.entity;
    out.selectedCameraIndex = selectedIndex;
    out.position = viewToWorld.m_translation;
    out.direction = viewToWorld.m_linear.row2;
    out.up = viewToWorld.m_linear.row1;
    out.verticalFovRadians = proxy.verticalFovRadians;
    out.zNear = proxy.zNear;
    out.useCustomIntrinsics = false;
    out.intrinsics = dm::float4(0.f);
    out.intrinsicsViewport = dm::float2(0.f);
    out.valid = true;
}

void ApplyCameraExposureToSettings(const CameraRenderProxy& proxy, PathTracerSettings& settings)
{
    if (proxy.projection != CameraProjectionKind::Perspective)
        return;

    ToneMappingParameters defaults;
    settings.ToneMappingParams.autoExposure =
        proxy.enableAutoExposure.value_or(defaults.autoExposure);
    settings.ToneMappingParams.exposureCompensation =
        proxy.exposureCompensation.value_or(defaults.exposureCompensation);
    settings.ToneMappingParams.exposureValue =
        proxy.exposureValue.value_or(defaults.exposureValue);
    settings.ToneMappingParams.exposureValueMin =
        proxy.exposureValueMin.value_or(defaults.exposureValueMin);
    settings.ToneMappingParams.exposureValueMax =
        proxy.exposureValueMax.value_or(defaults.exposureValueMax);
}

} // namespace

CameraRenderProxy makeCameraRenderProxy(
    ecs::Entity entity,
    const CameraComponent& component,
    const GlobalTransformComponent& global)
{
    CameraRenderProxy proxy;
    proxy.entity = entity;
    proxy.transform = global.transform;

    if (const PerspectiveCameraData* pers = tryGetPerspectiveCameraData(component))
    {
        proxy.projection = CameraProjectionKind::Perspective;
        proxy.zNear = pers->zNear;
        proxy.zFar = pers->zFar;
        proxy.verticalFovRadians = pers->verticalFov;
        proxy.aspectRatio = pers->aspectRatio;
        proxy.enableAutoExposure = pers->enableAutoExposure;
        proxy.exposureCompensation = pers->exposureCompensation;
        proxy.exposureValue = pers->exposureValue;
        proxy.exposureValueMin = pers->exposureValueMin;
        proxy.exposureValueMax = pers->exposureValueMax;
        return proxy;
    }

    if (const OrthographicCameraData* ortho = tryGetOrthographicCameraData(component))
    {
        proxy.projection = CameraProjectionKind::Orthographic;
        proxy.zNear = ortho->zNear;
        proxy.zFar = ortho->zFar;
        proxy.xMag = ortho->xMag;
        proxy.yMag = ortho->yMag;
    }
    return proxy;
}

void applyCameraRenderProxyToController(
    const CameraRenderProxy& proxy,
    CameraController& camera,
    PathTracerSettings* settings)
{
    if (proxy.projection != CameraProjectionKind::Perspective)
        return;

    ActiveCameraRenderProxy active;
    FillActiveCameraFromPerspectiveProxy(proxy, camera.selectedCameraIndex(), active);
    camera.camera().lookTo(active.position, active.direction, active.up);
    camera.setVerticalFOV(active.verticalFovRadians);
    camera.setZNear(active.zNear);
    camera.clearIntrinsics();

    if (settings)
        ApplyCameraExposureToSettings(proxy, *settings);
}

void extractSceneRenderData(
    SceneEntityWorld& entityWorld,
    SceneRenderData& inout,
    uint32_t frameIndex,
    SceneRenderExtractFlags flags)
{
    ecs::World& world = entityWorld.world();

    const bool needFull =
        flags.structureChanged
        || inout.meshInstances.empty();

    if (needFull)
    {
        // Keep active camera/settings; callers preserve them across republish.
        const ActiveCameraRenderProxy camera = inout.camera;
        const RenderSettingsSnapshot renderSettings = inout.renderSettings;
        inout.clear();
        inout.camera = camera;
        inout.renderSettings = renderSettings;

        ExtractMeshInstancesFull(world, inout);
        ExtractSkinnedMeshes(world, inout, frameIndex);
        ExtractLightsFull(world, inout);
        ExtractCameras(entityWorld, world, inout);
        ExtractGaussianSplats(world, inout);
        ExtractAnimationEntities(world, inout);
        return;
    }

    if (flags.transformsChanged)
    {
        if (!ExtractMeshInstancesTransforms(world, inout))
        {
            ExtractMeshInstancesFull(world, inout);
        }
        ExtractLightsTransforms(world, inout);
    }

    // Skinned joints track animation every frame even when hierarchy is idle.
    ExtractSkinnedMeshes(world, inout, frameIndex);
    // Cameras are few; refresh every frame so animated scene cams stay current.
    ExtractCameras(entityWorld, world, inout);
    // Gaussian splats are few; refresh transforms and enabled state every frame.
    ExtractGaussianSplats(world, inout);
}

void extractSessionRenderState(const SessionRenderExtractInputs& inputs, SceneRenderData& out)
{
    if (inputs.settings)
    {
        out.renderSettings.settings = *inputs.settings;
        inputs.settings->ResetAccumulation = false;
        inputs.settings->ResetRealtimeCaches = false;
        inputs.settings->NRDModeChanged = false;
    }

    if (inputs.runtime)
    {
        out.renderSettings.invalidation = inputs.runtime->Invalidation;
        out.renderSettings.picking = inputs.runtime->Picking;
    }

    out.renderSettings.gaussianSplatTemporalReset = inputs.gaussianSplatTemporalReset;
    out.renderSettings.sceneTime = inputs.sceneTime;

    if (inputs.gaussianSplatPasses)
    {
        for (GaussianSplatRenderProxy& proxy : out.gaussianSplats)
        {
            proxy.pass.reset();
            for (const render::SceneGaussianSplatPasses::SceneObject& object : inputs.gaussianSplatPasses->objects())
            {
                if (object.entity == proxy.entity)
                {
                    proxy.pass = object.pass;
                    break;
                }
            }
        }
    }

    if (!inputs.camera)
        return;

    const CameraController& freeCamera = *inputs.camera;
    const uint32_t selectedIndex = freeCamera.selectedCameraIndex();

    // selectedIndex 0 = free camera; 1..N = CameraRenderProxy[N-1].
    if (selectedIndex > 0)
    {
        const uint32_t proxyIndex = selectedIndex - 1;
        if (proxyIndex < out.cameras.size()
            && out.cameras[proxyIndex].projection == CameraProjectionKind::Perspective)
        {
            FillActiveCameraFromPerspectiveProxy(out.cameras[proxyIndex], selectedIndex, out.camera);
            ApplyCameraExposureToSettings(out.cameras[proxyIndex], out.renderSettings.settings);
            return;
        }
    }

    FillActiveCameraFromFreeController(freeCamera, out.camera);
}

} // namespace caustica::scene

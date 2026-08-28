#include <scene/SceneRenderExtract.h>
#include <scene/SceneRenderData.h>
#include <scene/SceneCameraAccess.h>
#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>
#include <scene/internal/RenderResourceAccess.h>

using caustica::scene::internal::RenderResourceAccess;

#include <render/core/CameraController.h>
#include <render/core/PathTracerSettings.h>
#include <render/core/ToneMappingParameters.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace caustica::scene
{

void FrameDynamicPacket::clear()
{
    meshInstances.clear();
    skinnedMeshes.clear();
    jointPalette.reset();
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

void SceneRenderData::clear()
{
    FrameDynamicPacket::clear();
    setStaticData(std::make_shared<SceneStaticPacket>());
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

const MeshRenderResourceSnapshot* SceneRenderData::findMesh(MeshRenderResourceId id) const
{
    const SceneStaticPacket& scene = staticData();
    const auto it = scene.meshSnapshotIndex.find(id);
    return it == scene.meshSnapshotIndex.end() || it->second >= scene.meshSnapshots.size()
        ? nullptr
        : &scene.meshSnapshots[it->second];
}

const MaterialRenderResourceSnapshot* SceneRenderData::findMaterial(MaterialRenderResourceId id) const
{
    const SceneStaticPacket& scene = staticData();
    const auto it = scene.materialSnapshotIndex.find(id);
    return it == scene.materialSnapshotIndex.end() || it->second >= scene.materialSnapshots.size()
        ? nullptr
        : &scene.materialSnapshots[it->second];
}

std::span<const dm::float4x4> SceneRenderData::jointMatrices(
    const SkinnedMeshRenderProxy& proxy) const
{
    if (!jointPalette || proxy.jointMatrixCount == 0
        || proxy.jointMatrixOffset > jointPalette->size()
        || proxy.jointMatrixCount > jointPalette->size() - proxy.jointMatrixOffset)
        return {};
    return std::span<const dm::float4x4>(
        jointPalette->data() + proxy.jointMatrixOffset,
        proxy.jointMatrixCount);
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
    const std::shared_ptr<MeshInfo>& mesh = ref.meshComp->mesh;
    proxy.meshId = RenderResourceAccess::meshId(mesh.get());
    proxy.globalMeshIndex = mesh ? mesh->globalMeshIndex : -1;
    proxy.geometryCount = mesh
        ? static_cast<uint32_t>(mesh->geometries.size())
        : 0;
    proxy.firstGlobalGeometryIndex =
        mesh && !mesh->geometries.empty() && mesh->geometries[0]
        ? mesh->geometries[0]->globalGeometryIndex
        : -1;
    proxy.meshType = mesh ? mesh->type : MeshType::Triangles;
    proxy.hasSkinPrototype = mesh && mesh->skinPrototype != nullptr;
    proxy.enabled = ref.meshComp->enabled;
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
    // Patch only entities whose global pose changed this ChangeDetection tick.
    // Requires: hierarchy refresh marks GlobalTransformComponent, and
    // endChangeDetectionFrame() runs AFTER extract (see Scene::extractAndPublish*).
    if (inout.meshInstances.size() != inout.meshInstanceEntities.size())
        return false;

    std::unordered_map<uint32_t, uint32_t> indexByEntity;
    indexByEntity.reserve(inout.meshInstances.size());
    for (uint32_t i = 0; i < inout.meshInstances.size(); ++i)
    {
        if (inout.meshInstances[i].entity != inout.meshInstanceEntities[i])
            return false;
        indexByEntity.emplace(static_cast<uint32_t>(inout.meshInstances[i].entity), i);
    }

    bool entitySetDrift = false;
    world.each<MeshInstanceComponent, GlobalTransformComponent, BoundsComponent, SceneContentComponent,
        ecs::Changed<GlobalTransformComponent>>(
        [&](ecs::Entity entity,
            MeshInstanceComponent& meshComp,
            GlobalTransformComponent& global,
            BoundsComponent& bounds,
            SceneContentComponent& content) {
            const auto it = indexByEntity.find(static_cast<uint32_t>(entity));
            if (it == indexByEntity.end())
            {
                entitySetDrift = true;
                return;
            }
            MeshInstanceRef ref{ entity, &meshComp, &global, &bounds, &content };
            FillMeshInstanceProxy(world, ref, inout.meshInstances[it->second]);
            inout.meshInstanceEntities[it->second] = entity;
        });

    if (entitySetDrift)
        return false;

    // Visibility can toggle without a transform dirty bit.
    for (MeshInstanceRenderProxy& proxy : inout.meshInstances)
    {
        if (const auto* meshComp = world.tryGet<MeshInstanceComponent>(proxy.entity))
            proxy.enabled = meshComp->enabled;
    }
    return true;
}

void ExtractSkinnedMeshes(ecs::World& world, SceneRenderData& out, uint32_t frameIndex)
{
    const size_t previousJointCount = out.jointPalette ? out.jointPalette->size() : 0;
    out.skinnedMeshes.clear();
    out.skinnedMeshInstanceEntities.clear();
    std::shared_ptr<std::vector<dm::float4x4>> jointPalette;

    world.each<SkinnedMeshComponent, MeshInstanceComponent, GlobalTransformComponent>(
        [&](ecs::Entity entity, SkinnedMeshComponent& skinned, MeshInstanceComponent& meshInstance,
            GlobalTransformComponent& ownerGlobal)
        {
            SkinnedMeshRenderProxy proxy;
            proxy.entity = entity;
            proxy.meshId = RenderResourceAccess::meshId(meshInstance.mesh.get());
            proxy.prototypeMeshId = RenderResourceAccess::meshId(skinned.prototypeMesh.get());
            proxy.transformFloat = ownerGlobal.transformFloat;
            if (const auto* name = world.get<NameComponent>(entity))
                proxy.debugName = name->value;

            const bool forceUpdate = skinned.lastUpdateFrameIndex == kForceSkinnedMeshUpdateFrameIndex;
            proxy.needsSkinningUpdate =
                forceUpdate || skinned.lastUpdateFrameIndex + 1 >= frameIndex;
            proxy.resetMotionHistory = skinned.resetMotionHistory;

            if (forceUpdate)
                skinned.lastUpdateFrameIndex = frameIndex;
            skinned.resetMotionHistory = false;

            if (!jointPalette)
            {
                jointPalette = std::make_shared<std::vector<dm::float4x4>>();
                jointPalette->reserve(previousJointCount);
            }
            const dm::daffine3 worldToRoot = inverse(ownerGlobal.transform);
            proxy.jointMatrixOffset = uint32_t(jointPalette->size());
            proxy.jointMatrixCount = uint32_t(skinned.joints.size());
            jointPalette->resize(jointPalette->size() + skinned.joints.size());
            dm::float4x4* jointMatrices = jointPalette->data() + proxy.jointMatrixOffset;

            for (size_t i = 0; i < skinned.joints.size(); ++i)
            {
                const SkinnedMeshJoint& joint = skinned.joints[i];
                const auto* jointGlobal = world.get<GlobalTransformComponent>(joint.jointEntity);
                if (!jointGlobal)
                {
                    jointMatrices[i] = dm::float4x4::identity();
                    continue;
                }

                const dm::float4x4 jointLocalToRoot =
                    dm::affineToHomogeneous(dm::affine3(jointGlobal->transform * worldToRoot));
                jointMatrices[i] = joint.inverseBindMatrix * jointLocalToRoot;
            }

            out.skinnedMeshes.push_back(std::move(proxy));
            out.skinnedMeshInstanceEntities.push_back(entity);
        });

    out.jointPalette = std::move(jointPalette);
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
            if (!light.enabled) return;
            extractLight(entity, light.color, {}, toLightData(light), global);
        });
    world.each<SpotLightComponent, GlobalTransformComponent>(
        [&](ecs::Entity entity, const SpotLightComponent& light, const GlobalTransformComponent& global)
        {
            if (!light.enabled) return;
            extractLight(entity, light.color, light.proxies, toLightData(light), global);
        });
    world.each<PointLightComponent, GlobalTransformComponent>(
        [&](ecs::Entity entity, const PointLightComponent& light, const GlobalTransformComponent& global)
        {
            if (!light.enabled) return;
            extractLight(entity, light.color, light.proxies, toLightData(light), global);
        });
    world.each<EnvironmentLightComponent, GlobalTransformComponent>(
        [&](ecs::Entity entity, const EnvironmentLightComponent& light, const GlobalTransformComponent& global)
        {
            if (!light.enabled) return;
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


void ExtractAnimationEntities(ecs::World& world, SceneRenderData& out)
{
    out.animationEntities.clear();
    world.each<AnimationComponent>([&](ecs::Entity entity, const AnimationComponent&) {
        out.animationEntities.push_back(entity);
    });
}

void ApplyCameraExposureToSettings(const CameraRenderProxy& proxy, PathTracerSettings& settings)
{
    if (proxy.projection != CameraProjectionKind::Perspective)
        return;

    ToneMappingParameters defaults;
    settings.ToneMappingParams.autoExposure =
        proxy.enableAutoExposure.value_or(defaults.autoExposure);
    settings.ToneMappingParams.toneMapOperator = defaults.toneMapOperator;
    if (proxy.toneMapOperator)
    {
        const std::string& op = *proxy.toneMapOperator;
        if (op == "Linear") settings.ToneMappingParams.toneMapOperator = ToneMapperOperator::Linear;
        else if (op == "Reinhard") settings.ToneMappingParams.toneMapOperator = ToneMapperOperator::Reinhard;
        else if (op == "ReinhardModified") settings.ToneMappingParams.toneMapOperator = ToneMapperOperator::ReinhardModified;
        else if (op == "HejiHableAlu") settings.ToneMappingParams.toneMapOperator = ToneMapperOperator::HejiHableAlu;
        else if (op == "HableUc2") settings.ToneMappingParams.toneMapOperator = ToneMapperOperator::HableUc2;
        else if (op == "Aces") settings.ToneMappingParams.toneMapOperator = ToneMapperOperator::Aces;
    }
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

void fillActiveCameraFromFreeController(const CameraController& camera, ActiveCameraRenderProxy& out)
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

void fillActiveCameraFromPerspectiveProxy(const CameraRenderProxy& proxy, uint32_t selectedIndex, ActiveCameraRenderProxy& out)
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
        proxy.toneMapOperator = pers->toneMapOperator;
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
    fillActiveCameraFromPerspectiveProxy(proxy, camera.selectedCameraIndex(), active);
    camera.camera().lookTo(active.position, active.direction, active.up);
    camera.setVerticalFOV(active.verticalFovRadians);
    camera.setZNear(active.zNear);
    camera.clearIntrinsics();

    if (settings)
        ApplyCameraExposureToSettings(proxy, *settings);
}

void ExtractMaterialSnapshots(
    const SceneEntityWorld& entityWorld,
    SceneStaticPacket& out)
{
    out.materialSnapshots.clear();
    out.materialSnapshotIndex.clear();
    out.materialSnapshots.reserve(entityWorld.getMaterials().size());
    for (const std::shared_ptr<Material>& material : entityWorld.getMaterials())
    {
        if (!material || !RenderResourceAccess::materialId(material.get()))
            continue;

        MaterialRenderResourceSnapshot snapshot;
        snapshot.id = RenderResourceAccess::materialId(material.get());
        snapshot.materialIndex = material->materialID;
        snapshot.debugName = material->name;
        snapshot.modelFileName = material->modelFileName;
        snapshot.materialIndexInModel = material->materialIndexInModel;
        snapshot.domain = material->domain;
        snapshot.baseOrDiffuseTexture = material->baseOrDiffuseTexture;
        snapshot.metalRoughOrSpecularTexture = material->metalRoughOrSpecularTexture;
        snapshot.normalTexture = material->normalTexture;
        snapshot.emissiveTexture = material->emissiveTexture;
        snapshot.occlusionTexture = material->occlusionTexture;
        snapshot.transmissionTexture = material->transmissionTexture;
        snapshot.opacityTexture = material->opacityTexture;
        snapshot.baseOrDiffuseColor = material->baseOrDiffuseColor;
        snapshot.specularColor = material->specularColor;
        snapshot.emissiveColor = material->emissiveColor;
        snapshot.emissiveIntensity = material->emissiveIntensity;
        snapshot.metalness = material->metalness;
        snapshot.roughness = material->roughness;
        snapshot.opacity = material->opacity;
        snapshot.alphaCutoff = material->alphaCutoff;
        snapshot.transmissionFactor = material->transmissionFactor;
        snapshot.normalTextureScale = material->normalTextureScale;
        snapshot.occlusionStrength = material->occlusionStrength;
        snapshot.normalTextureTransformScale = material->normalTextureTransformScale;
        snapshot.useSpecularGlossModel = material->useSpecularGlossModel;
        snapshot.enableBaseOrDiffuseTexture = material->enableBaseOrDiffuseTexture;
        snapshot.enableMetalRoughOrSpecularTexture = material->enableMetalRoughOrSpecularTexture;
        snapshot.enableNormalTexture = material->enableNormalTexture;
        snapshot.enableEmissiveTexture = material->enableEmissiveTexture;
        snapshot.enableOcclusionTexture = material->enableOcclusionTexture;
        snapshot.enableTransmissionTexture = material->enableTransmissionTexture;
        snapshot.enableOpacityTexture = material->enableOpacityTexture;
        snapshot.doubleSided = material->doubleSided;
        snapshot.metalnessInRedChannel = material->metalnessInRedChannel;
        snapshot.enableSubsurfaceScattering = material->enableSubsurfaceScattering;
        snapshot.subsurface = material->subsurface;
        snapshot.enableHair = material->enableHair;
        snapshot.hair = material->hair;
        out.materialSnapshotIndex.emplace(
            snapshot.id,
            static_cast<uint32_t>(out.materialSnapshots.size()));
        out.materialSnapshots.push_back(std::move(snapshot));
    }
}

void ExtractMeshSnapshots(const SceneEntityWorld& entityWorld, SceneStaticPacket& out)
{
    out.meshSnapshots.clear();
    out.meshSnapshotIndex.clear();
    out.meshSnapshots.reserve(entityWorld.getMeshes().size());

    // glTF importers intentionally let many MeshInfo records share one BufferGroup.
    // Deep-copying that whole group once per mesh turns a few hundred MB of source
    // data into tens of GB for scenes with hundreds of primitives (e.g. kitchen).
    // Keep the render snapshot immutable, but copy each distinct authoring buffer
    // group only once and share its upload blob between mesh-range snapshots.
    std::unordered_map<const BufferGroup*, std::shared_ptr<const MeshUploadBlob>>
        uploadByBufferGroup;
    uploadByBufferGroup.reserve(entityWorld.getMeshes().size());

    for (const std::shared_ptr<MeshInfo>& source : entityWorld.getMeshes())
    {
        if (!source || !RenderResourceAccess::meshId(source.get()))
            continue;
        MeshRenderResourceSnapshot mesh;
        mesh.id = RenderResourceAccess::meshId(source.get());
        mesh.debugName = source->name;
        mesh.type = source->type;
        mesh.objectSpaceBounds = source->objectSpaceBounds;
        mesh.indexOffset = source->indexOffset;
        mesh.vertexOffset = source->vertexOffset;
        mesh.totalIndices = source->totalIndices;
        mesh.totalVertices = source->totalVertices;
        mesh.globalMeshIndex = source->globalMeshIndex;
        mesh.isMorphTargetAnimationMesh = source->isMorphTargetAnimationMesh;
        mesh.isSkinPrototype = source->isSkinPrototype;
        mesh.hasSkinPrototype = source->skinPrototype != nullptr;
        mesh.hasDeformationSourcePositions = !source->DeformationSourcePositionIndices.empty();
        if (source->buffers)
        {
            const BufferGroup* const bufferKey = source->buffers.get();
            auto uploadIt = uploadByBufferGroup.find(bufferKey);
            if (uploadIt == uploadByBufferGroup.end())
            {
                auto upload = std::make_shared<MeshUploadBlob>();
                upload->indexData = source->buffers->indexData;
                upload->positionData = source->buffers->positionData;
                upload->texcoord1Data = source->buffers->texcoord1Data;
                upload->texcoord2Data = source->buffers->texcoord2Data;
                upload->normalData = source->buffers->normalData;
                upload->tangentData = source->buffers->tangentData;
                upload->jointData = source->buffers->jointData;
                upload->weightData = source->buffers->weightData;
                upload->radiusData = source->buffers->radiusData;
                uploadIt = uploadByBufferGroup.emplace(bufferKey, std::move(upload)).first;
            }
            mesh.upload = uploadIt->second;
        }
        mesh.geometries.reserve(source->geometries.size());
        for (const std::shared_ptr<MeshGeometry>& sourceGeometry : source->geometries)
        {
            if (!sourceGeometry)
                continue;
            GeometryRenderResourceSnapshot geometry;
            geometry.id = RenderResourceAccess::geometryId(sourceGeometry.get());
            geometry.materialId = RenderResourceAccess::materialId(sourceGeometry->material.get());
            geometry.materialIndex = sourceGeometry->material
                ? sourceGeometry->material->materialID
                : ~0u;
            geometry.materialDomain = sourceGeometry->material
                ? sourceGeometry->material->domain
                : MaterialDomain::Opaque;
            geometry.objectSpaceBounds = sourceGeometry->objectSpaceBounds;
            geometry.indexOffsetInMesh = sourceGeometry->indexOffsetInMesh;
            geometry.vertexOffsetInMesh = sourceGeometry->vertexOffsetInMesh;
            geometry.numIndices = sourceGeometry->numIndices;
            geometry.numVertices = sourceGeometry->numVertices;
            geometry.globalGeometryIndex = sourceGeometry->globalGeometryIndex;
            geometry.type = sourceGeometry->type;
            mesh.geometries.push_back(std::move(geometry));
        }
        out.meshSnapshotIndex.emplace(
            mesh.id,
            static_cast<uint32_t>(out.meshSnapshots.size()));
        out.meshSnapshots.push_back(std::move(mesh));
    }
}

void extractGaussianSplatProxies(SceneEntityWorld& entityWorld, SceneRenderData& out)
{
    out.gaussianSplats.clear();
    entityWorld.world().each<GaussianSplatComponent, GlobalTransformComponent>(
        [&](ecs::Entity entity, const GaussianSplatComponent& splat, const GlobalTransformComponent& global) {
            GaussianSplatRenderProxy proxy;
            proxy.entity = entity;
            proxy.enabled = splat.splat.enabled;
            proxy.objectToWorld = global.transformFloat;
            out.gaussianSplats.push_back(std::move(proxy));
        });
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
        uint64_t resourceBindingRevision = inout.staticData().resourceBindingRevision + 1;
        if (resourceBindingRevision == 0)
            resourceBindingRevision = 1;
        // A replacement static generation is installed below; clear only the
        // per-frame packet to avoid allocating an unused empty static packet.
        inout.FrameDynamicPacket::clear();
        inout.camera = camera;
        inout.renderSettings = renderSettings;
        auto staticPacket = std::make_shared<SceneStaticPacket>();
        staticPacket->resourceBindingRevision = resourceBindingRevision;
        staticPacket->geometryCount = entityWorld.getGeometryCount();
        ExtractMeshSnapshots(entityWorld, *staticPacket);
        ExtractMaterialSnapshots(entityWorld, *staticPacket);
        inout.setStaticData(std::move(staticPacket));

        ExtractMeshInstancesFull(world, inout);
        ExtractSkinnedMeshes(world, inout, frameIndex);
        ExtractLightsFull(world, inout);
        ExtractCameras(entityWorld, world, inout);
        ExtractAnimationEntities(world, inout);
        return;
    }

    if (flags.transformsChanged)
    {
        if (!ExtractMeshInstancesTransforms(world, inout))
            ExtractMeshInstancesFull(world, inout);
    }
    else
    {
        // Visibility can toggle without a transform/structure dirty bit.
        for (MeshInstanceRenderProxy& proxy : inout.meshInstances)
        {
            if (const auto* meshComp = world.tryGet<MeshInstanceComponent>(proxy.entity))
                proxy.enabled = meshComp->enabled;
        }
    }

    if (flags.transformsChanged || flags.lightsChanged)
        ExtractLightsTransforms(world, inout);

    // Skinned joints track animation every frame even when hierarchy is idle.
    ExtractSkinnedMeshes(world, inout, frameIndex);
    // Cameras are few; refresh every frame so animated scene cams stay current.
    ExtractCameras(entityWorld, world, inout);
    // Material snapshots are structure-owned; skip the full copy on transform-only frames.
    // Leaf types (GaussianSplat, …): Extract schedule systems after this core pass.
}

void extractFrameRenderState(const FrameExtractInputs& inputs, SceneRenderData& out)
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

    // Pure copy — free vs scene resolve runs in ResolveActiveCamera (PostUpdate).
    if (inputs.activeCamera)
        out.camera = *inputs.activeCamera;
}

} // namespace caustica::scene

#pragma once

#include <ecs/Entity.h>
#include <math/math.h>
#include <render/RenderRuntimeState.h>
#include <render/core/PathTracerSettings.h>
#include <scene/SceneContent.h>
#include <scene/SceneEcs.h>
#include <scene/SceneTypes.h>
#include <shaders/material_cb.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace caustica
{
class CameraController;
}

namespace caustica::scene
{
    // Canonical render-thread scene snapshot (UE-style render proxies).
    //
    // Contract:
    //   Logic thread (Bevy-style): mutate SceneEntityWorld / resources only; never upload GPU.
    //   Structure edits call Scene::requestGpuStructureSync(); Extract flushes mesh/AS
    //   then builds *RenderProxy lists + ActiveCamera / RenderSettings into SceneRenderSnapshot.
    //   Render thread: Scene::getRenderData() under beginGpuReadFrame, or
    //   getRenderDataForFrame(frameIndex) — never touches live ECS components.
    //
    // Proxy inventory (must stay complete for anything RT reads per frame):
    //   MeshInstanceRenderProxy, SkinnedMeshRenderProxy, LightRenderProxy,
    //   CameraRenderProxy (+ resolved ActiveCameraRenderProxy), GaussianSplatRenderProxy,
    //   RenderSettingsSnapshot.
    // Free-camera pose / selected index enter via SessionRenderExtractInputs; scene cameras
    // are resolved from CameraRenderProxy during Extract, not by RT querying ECS.

    struct MeshInstanceRenderProxy
    {
        ecs::Entity entity = ecs::NullEntity;
        int instanceIndex = -1;
        int geometryInstanceIndex = -1;
        MeshRenderResourceId meshId;
        int globalMeshIndex = -1;
        int firstGlobalGeometryIndex = -1;
        uint32_t geometryCount = 0;
        MeshType meshType = MeshType::Triangles;
        bool hasSkinPrototype = false;
        dm::affine3 transformFloat = dm::affine3::identity();
        dm::affine3 previousTransformFloat = dm::affine3::identity();
        dm::box3 globalBounds = dm::box3::empty();
        SceneContentFlags leafContent = SceneContentFlags::None;
        ecs::Entity proxiedAnalyticLight = ecs::NullEntity;
        ecs::Entity parentLightEntity = ecs::NullEntity;
    };

    struct SkinnedMeshRenderProxy
    {
        ecs::Entity entity = ecs::NullEntity;
        MeshRenderResourceId meshId;
        MeshRenderResourceId prototypeMeshId;
        dm::affine3 transformFloat = dm::affine3::identity();
        std::string debugName;
        std::vector<dm::float4x4> jointMatrices;
        bool needsSkinningUpdate = false;
    };

    // Analytic light fields + world transform. Operate on this directly — do not convert back to ECS.
    struct LightRenderProxy
    {
        ecs::Entity entity = ecs::NullEntity;
        dm::float3 color = dm::colors::white;
        std::vector<std::string> proxies;
        LightData data;
        dm::daffine3 transform = dm::daffine3::identity();
    };

    enum class CameraProjectionKind : uint8_t
    {
        Perspective,
        Orthographic,
    };

    // One ECS CameraComponent + GlobalTransform, extracted for the render world.
    struct CameraRenderProxy
    {
        ecs::Entity entity = ecs::NullEntity;
        CameraProjectionKind projection = CameraProjectionKind::Perspective;
        dm::daffine3 transform = dm::daffine3::identity();

        float zNear = 1.f;
        std::optional<float> zFar;

        // Perspective
        float verticalFovRadians = 1.f;
        std::optional<float> aspectRatio;
        std::optional<bool> enableAutoExposure;
        std::optional<float> exposureCompensation;
        std::optional<float> exposureValue;
        std::optional<float> exposureValueMin;
        std::optional<float> exposureValueMax;

        // Orthographic
        float xMag = 1.f;
        float yMag = 1.f;
    };

    // Resolved camera the render thread applies for this frame (free cam or selected scene cam).
    struct ActiveCameraRenderProxy
    {
        // False until Extract fills from free CameraController or a CameraRenderProxy.
        // Structure-only republish (runtime import) must not apply defaults to the live camera.
        bool valid = false;
        ecs::Entity sourceEntity = ecs::NullEntity; // NullEntity => free camera (index 0)
        uint32_t selectedCameraIndex = 0;
        dm::float3 position = { 0.f, 0.f, 0.f };
        dm::float3 direction = { 0.f, 0.f, -1.f };
        dm::float3 up = { 0.f, 1.f, 0.f };
        float verticalFovRadians = dm::radians(60.f);
        float zNear = 0.001f;
        bool useCustomIntrinsics = false;
        dm::float4 intrinsics = { 0.f, 0.f, 0.f, 0.f };
        dm::float2 intrinsicsViewport = { 0.f, 0.f };
    };

    // Backward-compatible name used by older call sites.
    using CameraSnapshot = ActiveCameraRenderProxy;

    // One ECS GaussianSplatComponent + GlobalTransform, extracted for the render world.
    // GPU pass ownership stays in SceneGaussianSplatPasses and is resolved by entity.
    struct GaussianSplatRenderProxy
    {
        ecs::Entity entity = ecs::NullEntity;
        bool enabled = true;
        dm::affine3 objectToWorld = dm::affine3::identity();
    };

    struct RenderSettingsSnapshot
    {
        PathTracerSettings settings;
        render::RenderInvalidationState invalidation;
        render::RenderPickState picking;
        bool gaussianSplatTemporalReset = false;
        double sceneTime = 0.0;
    };

    struct MaterialRenderResourceSnapshot
    {
        MaterialRenderResourceId id;
        uint32_t materialIndex = 0;
        std::string debugName;
        std::string modelFileName;
        int materialIndexInModel = -1;
        MaterialDomain domain = MaterialDomain::Opaque;
        Handle<ImageAsset> baseOrDiffuseTexture;
        Handle<ImageAsset> metalRoughOrSpecularTexture;
        Handle<ImageAsset> normalTexture;
        Handle<ImageAsset> emissiveTexture;
        Handle<ImageAsset> occlusionTexture;
        Handle<ImageAsset> transmissionTexture;
        Handle<ImageAsset> opacityTexture;
        dm::float3 baseOrDiffuseColor = 1.f;
        dm::float3 specularColor = 0.f;
        dm::float3 emissiveColor = 0.f;
        float emissiveIntensity = 1.f;
        float metalness = 0.f;
        float roughness = 0.f;
        float opacity = 1.f;
        float alphaCutoff = 0.5f;
        float transmissionFactor = 0.f;
        float normalTextureScale = 1.f;
        float occlusionStrength = 1.f;
        dm::float2 normalTextureTransformScale = 1.f;
        bool useSpecularGlossModel = false;
        bool enableBaseOrDiffuseTexture = true;
        bool enableMetalRoughOrSpecularTexture = true;
        bool enableNormalTexture = true;
        bool enableEmissiveTexture = true;
        bool enableOcclusionTexture = true;
        bool enableTransmissionTexture = true;
        bool enableOpacityTexture = true;
        bool doubleSided = false;
        bool metalnessInRedChannel = false;
        MaterialConstants constants = {};
        MaterialConstants bindlessConstants = {};
    };

    // Deep-copied only when structure is extracted. The immutable blob may be shared by
    // the triple-buffered snapshots and never contains scene authoring objects.
    struct MeshUploadBlob
    {
        std::vector<uint32_t> indexData;
        std::vector<dm::float3> positionData;
        std::vector<dm::float2> texcoord1Data;
        std::vector<dm::float2> texcoord2Data;
        std::vector<uint32_t> normalData;
        std::vector<uint32_t> tangentData;
        std::vector<dm::vector<uint16_t, 4>> jointData;
        std::vector<dm::float4> weightData;
        std::vector<float> radiusData;
    };

    struct GeometryRenderResourceSnapshot
    {
        GeometryRenderResourceId id;
        MaterialRenderResourceId materialId;
        uint32_t materialIndex = ~0u;
        dm::box3 objectSpaceBounds = dm::box3::empty();
        uint32_t indexOffsetInMesh = 0;
        uint32_t vertexOffsetInMesh = 0;
        uint32_t numIndices = 0;
        uint32_t numVertices = 0;
        int globalGeometryIndex = -1;
        MeshGeometryPrimitiveType type = MeshGeometryPrimitiveType::Triangles;
    };

    struct MeshRenderResourceSnapshot
    {
        MeshRenderResourceId id;
        std::string debugName;
        MeshType type = MeshType::Triangles;
        dm::box3 objectSpaceBounds = dm::box3::empty();
        uint32_t indexOffset = 0;
        uint32_t vertexOffset = 0;
        uint32_t totalIndices = 0;
        uint32_t totalVertices = 0;
        int globalMeshIndex = -1;
        bool isMorphTargetAnimationMesh = false;
        bool isSkinPrototype = false;
        bool hasSkinPrototype = false;
        bool hasDeformationSourcePositions = false;
        std::shared_ptr<const MeshUploadBlob> upload;
        std::vector<GeometryRenderResourceSnapshot> geometries;
    };

    // Logic-thread session inputs consumed during Extract (not ECS).
    // Free-camera pose / selection come from CameraController; PathTracerSettings is copied
    // into RenderSettingsSnapshot (one-shot flags cleared after copy). Scene cameras are
    // taken from CameraRenderProxy when selectedCameraIndex > 0.
    struct SessionRenderExtractInputs
    {
        const class CameraController* camera = nullptr;
        PathTracerSettings* settings = nullptr; // non-const: one-shot flags cleared after copy
        const render::RenderRuntimeState* runtime = nullptr;
        double sceneTime = 0.0;
        bool gaussianSplatTemporalReset = false;
    };

    class SceneRenderData
    {
    public:
        void clear();

        [[nodiscard]] const LightRenderProxy* findLight(ecs::Entity entity) const;
        [[nodiscard]] const CameraRenderProxy* findCamera(ecs::Entity entity) const;
        [[nodiscard]] const MeshRenderResourceSnapshot* findMesh(MeshRenderResourceId id) const;
        [[nodiscard]] const MaterialRenderResourceSnapshot* findMaterial(MaterialRenderResourceId id) const;

        std::vector<MeshInstanceRenderProxy> meshInstances;
        std::vector<SkinnedMeshRenderProxy> skinnedMeshes;
        std::vector<LightRenderProxy> lights;
        std::vector<CameraRenderProxy> cameras;
        std::vector<GaussianSplatRenderProxy> gaussianSplats;

        std::vector<MeshRenderResourceSnapshot> meshSnapshots;
        std::vector<MaterialRenderResourceSnapshot> materialSnapshots;
        std::unordered_map<MeshRenderResourceId, uint32_t, MeshRenderResourceId::Hash>
            meshSnapshotIndex;
        std::unordered_map<MaterialRenderResourceId, uint32_t, MaterialRenderResourceId::Hash>
            materialSnapshotIndex;
        uint64_t resourceBindingRevision = 0;
        size_t geometryCount = 0;

        ActiveCameraRenderProxy camera;
        RenderSettingsSnapshot renderSettings;

        std::vector<ecs::Entity> meshInstanceEntities;
        std::vector<ecs::Entity> skinnedMeshInstanceEntities;
        std::vector<ecs::Entity> lightEntities;
        std::vector<ecs::Entity> cameraEntities;
        std::vector<ecs::Entity> animationEntities;
    };

} // namespace caustica::scene

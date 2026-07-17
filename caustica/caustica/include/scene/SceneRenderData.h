#pragma once

#include <ecs/Entity.h>
#include <math/math.h>
#include <render/RenderRuntimeState.h>
#include <render/core/PathTracerSettings.h>
#include <render/passes/gaussian/GaussianSplatPass.h>
#include <scene/SceneContent.h>
#include <scene/SceneEcs.h>
#include <scene/SceneTypes.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class GaussianSplatPass;

namespace caustica
{
class CameraController;
}

namespace caustica::render
{
class SceneGaussianSplatPasses;
}

namespace caustica::scene
{
    // Canonical render-thread scene snapshot (UE-style render proxies).
    //
    // Contract:
    //   Logic thread (Bevy-style): mutate SceneEntityWorld / resources only; never upload GPU.
    //   Structure edits call Scene::requestGpuStructureSync(); Extract flushes mesh/AS
    //   then builds *RenderProxy lists + ActiveCamera / RenderSettings into SceneRenderSnapshot.
    //   Render thread: Scene::getRenderData() only — never touches live ECS components.
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
        int globalMeshIndex = -1;
        int firstGlobalGeometryIndex = -1;
        uint32_t geometryCount = 0;
        MeshType meshType = MeshType::Triangles;
        bool hasSkinPrototype = false;
        // Transitional lifetime pin for render resources still owned by MeshInfo.
        // Render code must use the immutable fields above whenever it does not need GPU handles.
        std::shared_ptr<MeshInfo> meshShared;
        dm::affine3 transformFloat = dm::affine3::identity();
        dm::affine3 previousTransformFloat = dm::affine3::identity();
        dm::box3 globalBounds = dm::box3::empty();
        SceneContentFlags leafContent = SceneContentFlags::None;
        ecs::Entity proxiedAnalyticLight = ecs::NullEntity;
        ecs::Entity parentLightEntity = ecs::NullEntity;
    };

    struct SkinnedMeshJointLineProxy
    {
        dm::float3 jointPosition = { 0.f, 0.f, 0.f };
        dm::float3 parentPosition = { 0.f, 0.f, 0.f };
        bool hasParent = false;
    };

    struct SkinnedMeshRenderProxy
    {
        ecs::Entity entity = ecs::NullEntity;
        std::shared_ptr<MeshInfo> mesh;
        std::shared_ptr<MeshInfo> prototypeMesh;
        dm::affine3 transformFloat = dm::affine3::identity();
        std::string debugName;
        std::vector<dm::float4x4> jointMatrices;
        std::vector<SkinnedMeshJointLineProxy> jointLines;
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
    // The pass is bound from SceneGaussianSplatPasses during session extract, never from ECS.
    struct GaussianSplatRenderProxy
    {
        ecs::Entity entity = ecs::NullEntity;
        bool enabled = true;
        dm::affine3 objectToWorld = dm::affine3::identity();
        std::shared_ptr<::GaussianSplatPass> pass;
    };

    struct RenderSettingsSnapshot
    {
        PathTracerSettings settings;
        render::RenderInvalidationState invalidation;
        render::RenderPickState picking;
        bool gaussianSplatTemporalReset = false;
        double sceneTime = 0.0;
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
        const render::SceneGaussianSplatPasses* gaussianSplatPasses = nullptr;
        double sceneTime = 0.0;
        bool gaussianSplatTemporalReset = false;
    };

    class SceneRenderData
    {
    public:
        void clear();

        [[nodiscard]] const LightRenderProxy* findLight(ecs::Entity entity) const;
        [[nodiscard]] const CameraRenderProxy* findCamera(ecs::Entity entity) const;

        std::vector<MeshInstanceRenderProxy> meshInstances;
        std::vector<SkinnedMeshRenderProxy> skinnedMeshes;
        std::vector<LightRenderProxy> lights;
        std::vector<CameraRenderProxy> cameras;
        std::vector<GaussianSplatRenderProxy> gaussianSplats;

        // Transitional lifetime snapshots. Render code may consume these stable
        // resource lists but must not query SceneEntityWorld/ResourceTracker.
        std::vector<std::shared_ptr<MeshInfo>> meshResources;
        std::vector<std::shared_ptr<Material>> materialResources;
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

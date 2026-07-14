#pragma once

#include <ecs/Entity.h>
#include <math/math.h>
#include <render/RenderRuntimeState.h>
#include <render/core/PathTracerSettings.h>
#include <scene/SceneContent.h>
#include <scene/SceneEcs.h>
#include <scene/SceneTypes.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace caustica::scene
{
    // Canonical render-thread scene snapshot (UE-style render proxies).
    // Logic thread: extractSceneRenderData → SceneRenderSnapshot.
    // Render thread: Scene::getRenderData() — never touches ECS components.

    struct MeshInstanceRenderProxy
    {
        ecs::Entity entity = ecs::NullEntity;
        int instanceIndex = -1;
        int geometryInstanceIndex = -1;
        MeshInfo* mesh = nullptr;
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

    struct CameraSnapshot
    {
        // False until extractSessionRenderState fills from CameraController.
        // Structure-only republish (runtime import) must not apply defaults to the live camera —
        // verticalFovDegrees stores radians despite the name; default 60 would be treated as 60 rad.
        bool valid = false;
        dm::float3 position = { 0.f, 0.f, 0.f };
        dm::float3 direction = { 0.f, 0.f, -1.f };
        dm::float3 up = { 0.f, 1.f, 0.f };
        float verticalFovDegrees = 60.f;
        float zNear = 0.001f;
        bool useCustomIntrinsics = false;
        dm::float4 intrinsics = { 0.f, 0.f, 0.f, 0.f };
        dm::float2 intrinsicsViewport = { 0.f, 0.f };
        uint32_t selectedCameraIndex = 0;
    };

    struct RenderSettingsSnapshot
    {
        PathTracerSettings settings;
        render::RenderInvalidationState invalidation;
        render::RenderPickState picking;
        bool gaussianSplatTemporalReset = false;
        double sceneTime = 0.0;
    };

    // Optional session inputs filled on the logic thread during Extract (not ECS).
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

        std::vector<MeshInstanceRenderProxy> meshInstances;
        std::vector<SkinnedMeshRenderProxy> skinnedMeshes;
        std::vector<LightRenderProxy> lights;

        CameraSnapshot camera;
        RenderSettingsSnapshot renderSettings;

        std::vector<ecs::Entity> meshInstanceEntities;
        std::vector<ecs::Entity> skinnedMeshInstanceEntities;
        std::vector<ecs::Entity> lightEntities;
        std::vector<ecs::Entity> cameraEntities;
        std::vector<ecs::Entity> animationEntities;
    };

} // namespace caustica::scene

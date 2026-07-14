#pragma once

#include <ecs/Entity.h>
#include <math/math.h>
#include <scene/SceneContent.h>
#include <scene/SceneTypes.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace caustica::scene
{
    // Canonical render-thread scene snapshot (UE-style render proxies).
    // Logic thread: extractSceneRenderData → SceneRenderSnapshot.
    // render thread: Scene::getRenderData() — never touches ECS components.
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
    };

    // Debug lines for JointsRenderPass (world-space endpoints).
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
        // Skinning matrices in root space (inverseBind * jointLocalToRoot), computed at Extract.
        std::vector<dm::float4x4> jointMatrices;
        std::vector<SkinnedMeshJointLineProxy> jointLines;
        bool needsSkinningUpdate = false;
    };

    class SceneRenderData
    {
    public:
        void clear();

        std::vector<MeshInstanceRenderProxy> meshInstances;
        std::vector<SkinnedMeshRenderProxy> skinnedMeshes;

        std::vector<ecs::Entity> meshInstanceEntities;
        std::vector<ecs::Entity> skinnedMeshInstanceEntities;
        std::vector<ecs::Entity> lightEntities;
        std::vector<ecs::Entity> cameraEntities;
        std::vector<ecs::Entity> animationEntities;
    };

} // namespace caustica::scene

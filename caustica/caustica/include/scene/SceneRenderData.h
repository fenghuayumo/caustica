#pragma once

#include <ecs/Entity.h>
#include <scene/SceneContent.h>
#include <scene/SceneEcs.h>
#include <scene/SceneObjects.h>
#include <memory>
#include <vector>

namespace caustica::scene
{
    // Canonical render-thread scene snapshot (UE-style render resource).
    // Logic thread: ExtractSceneRenderData → SceneRenderSnapshot.
    // Render thread: Scene::GetRenderData() or render::tryGetRenderSceneData(renderWorld).
    struct MeshInstanceRenderProxy
    {
        ecs::Entity entity = ecs::NullEntity;
        int instanceIndex = -1;
        int geometryInstanceIndex = -1;
        MeshInfo* mesh = nullptr;
        std::shared_ptr<MeshInfo> meshShared;
        dm::affine3 transformFloat = dm::affine3::identity();
        dm::box3 globalBounds = dm::box3::empty();
        SceneContentFlags leafContent = SceneContentFlags::None;
    };

    struct SkinnedMeshRenderProxy
    {
        ecs::Entity entity = ecs::NullEntity;
        SkinnedMeshComponent* skinned = nullptr;
        MeshInstanceComponent* meshInstance = nullptr;
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

#pragma once

#include <scene/SceneObjects.h>
#include <scene/SceneAnimation.h>
#include <scene/ResourceTracker.h>
#include <ecs/Entity.h>
#include <functional>
#include <memory>
#include <vector>

namespace caustica
{
    template<typename T>
    using SceneResourceCallback = std::function<void(const std::shared_ptr<T>&)>;

    // Tracks unique meshes and materials referenced by a set of scene objects,
    // and maintains flat lists of mesh instances, lights, cameras, and animations.
    // Derived classes may override Register/Unregister to extend tracking behaviour.
    class SceneResources
    {
    public:
        SceneResources() = default;
        virtual ~SceneResources() = default;

        SceneResourceCallback<MeshInfo>  OnMeshAdded;
        SceneResourceCallback<MeshInfo>  OnMeshRemoved;
        SceneResourceCallback<Material>  OnMaterialAdded;
        SceneResourceCallback<Material>  OnMaterialRemoved;

        [[nodiscard]] const ResourceTracker<Material>& GetMaterials()              const { return m_Materials; }
        [[nodiscard]] const ResourceTracker<MeshInfo>& GetMeshes()                 const { return m_Meshes; }
        [[nodiscard]] size_t GetGeometryCount()                                    const { return m_GeometryCount; }
        [[nodiscard]] size_t GetMaxGeometryCountPerMesh()                          const { return m_MaxGeometryCountPerMesh; }
        [[nodiscard]] size_t GetGeometryInstancesCount()                           const { return m_GeometryInstancesCount; }

        [[nodiscard]] const std::vector<ecs::Entity>& GetMeshInstanceEntities() const { return m_MeshInstanceEntities; }
        [[nodiscard]] const std::vector<ecs::Entity>& GetSkinnedMeshInstanceEntities() const { return m_SkinnedMeshInstanceEntities; }
        [[nodiscard]] const std::vector<ecs::Entity>& GetLightEntities() const { return m_LightEntities; }
        [[nodiscard]] const std::vector<ecs::Entity>& GetCameraEntities() const { return m_CameraEntities; }
        [[nodiscard]] const std::vector<std::shared_ptr<SceneAnimation>>&      GetAnimations()           const { return m_Animations; }

        void RegisterLightEntity(ecs::Entity entity);
        void UnregisterLightEntity(ecs::Entity entity);
        void RegisterCameraEntity(ecs::Entity entity);
        void UnregisterCameraEntity(ecs::Entity entity);

        // Typed Register/Unregister. Derived classes may override to extend tracking.
        void RegisterMeshInstanceEntity(ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh, bool skinned);
        void UnregisterMeshInstanceEntity(ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh, bool skinned);

        virtual void RegisterLeaf(const std::shared_ptr<SceneAnimation>& leaf);

        virtual void UnregisterLeaf(const std::shared_ptr<SceneAnimation>& leaf);

        SceneResources(const SceneResources&) = delete;
        SceneResources& operator=(const SceneResources&) = delete;

    protected:
        ResourceTracker<Material>  m_Materials;
        ResourceTracker<MeshInfo>  m_Meshes;
        size_t m_GeometryCount = 0;
        size_t m_MaxGeometryCountPerMesh = 0;
        size_t m_GeometryInstancesCount = 0;
        std::vector<ecs::Entity> m_MeshInstanceEntities;
        std::vector<ecs::Entity> m_SkinnedMeshInstanceEntities;
        std::vector<ecs::Entity> m_LightEntities;
        std::vector<ecs::Entity> m_CameraEntities;
        std::vector<std::shared_ptr<SceneAnimation>>      m_Animations;
    };

} // namespace caustica

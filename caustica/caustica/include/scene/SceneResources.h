#pragma once

#include <scene/SceneObjects.h>
#include <scene/SceneAnimation.h>
#include <scene/ResourceTracker.h>
#include <ecs/Entity.h>
#include <functional>
#include <memory>

namespace caustica
{
    template<typename T>
    using SceneResourceCallback = std::function<void(const std::shared_ptr<T>&)>;

    // Tracks unique meshes and materials referenced by scene mesh instances.
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

        void RegisterMeshInstanceEntity(ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh, bool skinned);
        void UnregisterMeshInstanceEntity(ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh, bool skinned);

        SceneResources(const SceneResources&) = delete;
        SceneResources& operator=(const SceneResources&) = delete;

    protected:
        ResourceTracker<Material>  m_Materials;
        ResourceTracker<MeshInfo>  m_Meshes;
        size_t m_GeometryCount = 0;
        size_t m_MaxGeometryCountPerMesh = 0;
        size_t m_GeometryInstancesCount = 0;
    };

} // namespace caustica

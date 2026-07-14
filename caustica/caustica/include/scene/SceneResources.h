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

        [[nodiscard]] const ResourceTracker<Material>& getMaterials()              const { return m_Materials; }
        [[nodiscard]] const ResourceTracker<MeshInfo>& getMeshes()                 const { return m_Meshes; }
        [[nodiscard]] size_t getGeometryCount()                                    const { return m_GeometryCount; }
        [[nodiscard]] size_t getMaxGeometryCountPerMesh()                          const { return m_MaxGeometryCountPerMesh; }
        [[nodiscard]] size_t getGeometryInstancesCount()                           const { return m_GeometryInstancesCount; }

        void registerMeshInstanceEntity(ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh, bool skinned);
        void unregisterMeshInstanceEntity(ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh, bool skinned);

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

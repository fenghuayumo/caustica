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

        [[nodiscard]] const ResourceTracker<Material>& getMaterials()              const { return m_materials; }
        [[nodiscard]] const ResourceTracker<MeshInfo>& getMeshes()                 const { return m_meshes; }
        [[nodiscard]] size_t getGeometryCount()                                    const { return m_geometryCount; }
        [[nodiscard]] size_t getMaxGeometryCountPerMesh()                          const { return m_maxGeometryCountPerMesh; }
        [[nodiscard]] size_t getGeometryInstancesCount()                           const { return m_geometryInstancesCount; }

        void registerMeshInstanceEntity(ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh, bool skinned);
        void unregisterMeshInstanceEntity(ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh, bool skinned);

        SceneResources(const SceneResources&) = delete;
        SceneResources& operator=(const SceneResources&) = delete;

    protected:
        ResourceTracker<Material>  m_materials;
        ResourceTracker<MeshInfo>  m_meshes;
        size_t m_geometryCount = 0;
        size_t m_maxGeometryCountPerMesh = 0;
        size_t m_geometryInstancesCount = 0;
    };

} // namespace caustica

#pragma once

#include <scene/SceneObjects.h>
#include <scene/SceneAnimation.h>
#include <scene/ResourceTracker.h>
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

        [[nodiscard]] const std::vector<std::shared_ptr<MeshInstance>>&        GetMeshInstances()        const { return m_MeshInstances; }
        [[nodiscard]] const std::vector<std::shared_ptr<SkinnedMeshInstance>>& GetSkinnedMeshInstances() const { return m_SkinnedMeshInstances; }
        [[nodiscard]] const std::vector<std::shared_ptr<Light>>&               GetLights()               const { return m_Lights; }
        [[nodiscard]] const std::vector<std::shared_ptr<SceneCamera>>&         GetCameras()              const { return m_Cameras; }
        [[nodiscard]] const std::vector<std::shared_ptr<SceneAnimation>>&      GetAnimations()           const { return m_Animations; }

        // Recalculates m_GeometryInstancesCount and per-instance index caches.
        // Call after all structural changes (adds/removes) are complete.
        void RefreshInstanceIndices();

        // Typed Register/Unregister. Derived classes may override to extend tracking.
        virtual void RegisterLeaf(const std::shared_ptr<MeshInstance>& leaf);
        virtual void RegisterLeaf(const std::shared_ptr<SceneCamera>& leaf);
        virtual void RegisterLeaf(const std::shared_ptr<Light>& leaf);
        virtual void RegisterLeaf(const std::shared_ptr<SceneAnimation>& leaf);

        virtual void UnregisterLeaf(const std::shared_ptr<MeshInstance>& leaf);
        virtual void UnregisterLeaf(const std::shared_ptr<SceneCamera>& leaf);
        virtual void UnregisterLeaf(const std::shared_ptr<Light>& leaf);
        virtual void UnregisterLeaf(const std::shared_ptr<SceneAnimation>& leaf);

        SceneResources(const SceneResources&) = delete;
        SceneResources& operator=(const SceneResources&) = delete;

    protected:
        ResourceTracker<Material>  m_Materials;
        ResourceTracker<MeshInfo>  m_Meshes;
        size_t m_GeometryCount = 0;
        size_t m_MaxGeometryCountPerMesh = 0;
        size_t m_GeometryInstancesCount = 0;
        std::vector<std::shared_ptr<MeshInstance>>        m_MeshInstances;
        std::vector<std::shared_ptr<SkinnedMeshInstance>> m_SkinnedMeshInstances;
        std::vector<std::shared_ptr<Light>>               m_Lights;
        std::vector<std::shared_ptr<SceneCamera>>         m_Cameras;
        std::vector<std::shared_ptr<SceneAnimation>>      m_Animations;
    };

} // namespace caustica

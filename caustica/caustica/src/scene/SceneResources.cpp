#include <scene/SceneResources.h>
#include <algorithm>

using namespace caustica;

// =============================================================================
// SceneResources – RegisterLeaf overloads
// =============================================================================

void SceneResources::RegisterLeaf(const std::shared_ptr<MeshInstance>& leaf)
{
    if (!leaf)
        return;

    const auto& mesh = leaf->GetMesh();
    if (mesh)
    {
        size_t geometryCount = 0;

        if (m_Meshes.AddRef(mesh))
        {
            geometryCount += mesh->geometries.size();
            m_GeometryCount += mesh->geometries.size();
            if (OnMeshAdded)
                OnMeshAdded(mesh);
        }

        for (const auto& geometry : mesh->geometries)
        {
            if (m_Materials.AddRef(geometry->material) && OnMaterialAdded)
                OnMaterialAdded(geometry->material);
        }

        if (mesh->skinPrototype)
        {
            if (m_Meshes.AddRef(mesh->skinPrototype))
            {
                geometryCount += mesh->skinPrototype->geometries.size();
                m_GeometryCount += mesh->skinPrototype->geometries.size();
                if (OnMeshAdded)
                    OnMeshAdded(mesh->skinPrototype);
            }
        }

        m_MaxGeometryCountPerMesh = std::max(m_MaxGeometryCountPerMesh, geometryCount);
    }

    m_MeshInstances.push_back(leaf);

    auto skinnedInstance = std::dynamic_pointer_cast<SkinnedMeshInstance>(leaf);
    if (skinnedInstance)
        m_SkinnedMeshInstances.push_back(skinnedInstance);
}

void SceneResources::RegisterLeaf(const std::shared_ptr<SceneCamera>& leaf)
{
    if (leaf)
        m_Cameras.push_back(leaf);
}

void SceneResources::RegisterLeaf(const std::shared_ptr<Light>& leaf)
{
    if (leaf)
        m_Lights.push_back(leaf);
}

void SceneResources::RegisterLeaf(const std::shared_ptr<SceneAnimation>& leaf)
{
    if (leaf)
        m_Animations.push_back(leaf);
}

// =============================================================================
// SceneResources – UnregisterLeaf overloads
// =============================================================================

void SceneResources::UnregisterLeaf(const std::shared_ptr<MeshInstance>& leaf)
{
    if (!leaf)
        return;

    const auto& mesh = leaf->GetMesh();
    if (mesh)
    {
        if (m_Meshes.Release(mesh))
        {
            m_GeometryCount -= mesh->geometries.size();
            if (OnMeshRemoved)
                OnMeshRemoved(mesh);
        }

        for (const auto& geometry : mesh->geometries)
        {
            if (m_Materials.Release(geometry->material) && OnMaterialRemoved)
                OnMaterialRemoved(geometry->material);
        }

        if (mesh->skinPrototype)
        {
            if (m_Meshes.Release(mesh->skinPrototype))
            {
                m_GeometryCount -= mesh->skinPrototype->geometries.size();
                if (OnMeshRemoved)
                    OnMeshRemoved(mesh->skinPrototype);
            }
        }
    }

    auto it = std::find(m_MeshInstances.begin(), m_MeshInstances.end(), leaf);
    if (it != m_MeshInstances.end())
        m_MeshInstances.erase(it);

    auto skinnedInstance = std::dynamic_pointer_cast<SkinnedMeshInstance>(leaf);
    if (skinnedInstance)
    {
        auto sit = std::find(m_SkinnedMeshInstances.begin(), m_SkinnedMeshInstances.end(), skinnedInstance);
        if (sit != m_SkinnedMeshInstances.end())
            m_SkinnedMeshInstances.erase(sit);
    }
}

void SceneResources::UnregisterLeaf(const std::shared_ptr<SceneCamera>& leaf)
{
    auto it = std::find(m_Cameras.begin(), m_Cameras.end(), leaf);
    if (it != m_Cameras.end())
        m_Cameras.erase(it);
}

void SceneResources::UnregisterLeaf(const std::shared_ptr<Light>& leaf)
{
    auto it = std::find(m_Lights.begin(), m_Lights.end(), leaf);
    if (it != m_Lights.end())
        m_Lights.erase(it);
}

void SceneResources::UnregisterLeaf(const std::shared_ptr<SceneAnimation>& leaf)
{
    auto it = std::find(m_Animations.begin(), m_Animations.end(), leaf);
    if (it != m_Animations.end())
        m_Animations.erase(it);
}

// =============================================================================
// SceneResources – RefreshInstanceIndices
// =============================================================================

void SceneResources::RefreshInstanceIndices()
{
    // SceneResources is declared friend in MeshInstance, so we can write the private indices.
    int instanceIndex = 0;
    int geometryInstanceIndex = 0;

    for (const auto& instance : m_MeshInstances)
    {
        instance->m_InstanceIndex = instanceIndex++;
        instance->m_GeometryInstanceIndex = geometryInstanceIndex;
        if (const auto& mesh = instance->GetMesh())
            geometryInstanceIndex += static_cast<int>(mesh->geometries.size());
    }

    m_GeometryInstancesCount = static_cast<size_t>(geometryInstanceIndex);
}

#include <scene/SceneResources.h>

#include <algorithm>

using namespace caustica;

void SceneResources::RegisterMeshInstanceEntity(
    ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh, bool skinned)
{
    if (!ecs::isValid(entity) || !mesh)
        return;

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
    m_MeshInstanceEntities.push_back(entity);
    if (skinned)
        m_SkinnedMeshInstanceEntities.push_back(entity);
}

void SceneResources::UnregisterMeshInstanceEntity(
    ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh, bool skinned)
{
    if (!ecs::isValid(entity))
        return;

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

    auto it = std::find(m_MeshInstanceEntities.begin(), m_MeshInstanceEntities.end(), entity);
    if (it != m_MeshInstanceEntities.end())
        m_MeshInstanceEntities.erase(it);

    if (skinned)
    {
        auto sit = std::find(m_SkinnedMeshInstanceEntities.begin(), m_SkinnedMeshInstanceEntities.end(), entity);
        if (sit != m_SkinnedMeshInstanceEntities.end())
            m_SkinnedMeshInstanceEntities.erase(sit);
    }
}

void SceneResources::RegisterLightEntity(ecs::Entity entity)
{
    if (ecs::isValid(entity))
        m_LightEntities.push_back(entity);
}

void SceneResources::UnregisterLightEntity(ecs::Entity entity)
{
    auto it = std::find(m_LightEntities.begin(), m_LightEntities.end(), entity);
    if (it != m_LightEntities.end())
        m_LightEntities.erase(it);
}

void SceneResources::RegisterCameraEntity(ecs::Entity entity)
{
    if (ecs::isValid(entity))
        m_CameraEntities.push_back(entity);
}

void SceneResources::UnregisterCameraEntity(ecs::Entity entity)
{
    auto it = std::find(m_CameraEntities.begin(), m_CameraEntities.end(), entity);
    if (it != m_CameraEntities.end())
        m_CameraEntities.erase(it);
}

void SceneResources::RegisterLeaf(const std::shared_ptr<SceneAnimation>& leaf)
{
    if (leaf)
        m_Animations.push_back(leaf);
}

void SceneResources::UnregisterLeaf(const std::shared_ptr<SceneAnimation>& leaf)
{
    auto it = std::find(m_Animations.begin(), m_Animations.end(), leaf);
    if (it != m_Animations.end())
        m_Animations.erase(it);
}

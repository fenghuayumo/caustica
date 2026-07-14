#include <scene/SceneResources.h>

#include <algorithm>

using namespace caustica;

void SceneResources::registerMeshInstanceEntity(
    ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh, bool /*skinned*/)
{
    if (!ecs::isValid(entity) || !mesh)
        return;

    size_t geometryCount = 0;

    if (m_Meshes.addRef(mesh))
    {
        geometryCount += mesh->geometries.size();
        m_GeometryCount += mesh->geometries.size();
        if (OnMeshAdded)
            OnMeshAdded(mesh);
    }

    for (const auto& geometry : mesh->geometries)
    {
        if (m_Materials.addRef(geometry->material) && OnMaterialAdded)
            OnMaterialAdded(geometry->material);
    }

    if (mesh->skinPrototype)
    {
        if (m_Meshes.addRef(mesh->skinPrototype))
        {
            geometryCount += mesh->skinPrototype->geometries.size();
            m_GeometryCount += mesh->skinPrototype->geometries.size();
            if (OnMeshAdded)
                OnMeshAdded(mesh->skinPrototype);
        }
    }

    m_MaxGeometryCountPerMesh = std::max(m_MaxGeometryCountPerMesh, geometryCount);
}

void SceneResources::unregisterMeshInstanceEntity(
    ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh, bool /*skinned*/)
{
    if (!ecs::isValid(entity))
        return;

    if (mesh)
    {
        if (m_Meshes.release(mesh))
        {
            m_GeometryCount -= mesh->geometries.size();
            if (OnMeshRemoved)
                OnMeshRemoved(mesh);
        }

        for (const auto& geometry : mesh->geometries)
        {
            if (m_Materials.release(geometry->material) && OnMaterialRemoved)
                OnMaterialRemoved(geometry->material);
        }

        if (mesh->skinPrototype)
        {
            if (m_Meshes.release(mesh->skinPrototype))
            {
                m_GeometryCount -= mesh->skinPrototype->geometries.size();
                if (OnMeshRemoved)
                    OnMeshRemoved(mesh->skinPrototype);
            }
        }
    }
}

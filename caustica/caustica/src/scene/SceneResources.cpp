#include <scene/SceneResources.h>

#include <algorithm>
#include <atomic>

using namespace caustica;

namespace
{

std::atomic<uint32_t> g_nextMeshRenderResourceId{0};
std::atomic<uint32_t> g_nextGeometryRenderResourceId{0};
std::atomic<uint32_t> g_nextMaterialRenderResourceId{0};

void EnsureMaterialRenderResourceId(const std::shared_ptr<Material>& material)
{
    if (material && !material->renderResourceId)
    {
        material->renderResourceId = scene::MaterialRenderResourceId::make(
            g_nextMaterialRenderResourceId.fetch_add(1, std::memory_order_relaxed), 0);
    }
}

void EnsureMeshRenderResourceIds(const std::shared_ptr<MeshInfo>& mesh)
{
    if (!mesh)
        return;

    if (!mesh->renderResourceId)
    {
        mesh->renderResourceId = scene::MeshRenderResourceId::make(
            g_nextMeshRenderResourceId.fetch_add(1, std::memory_order_relaxed), 0);
    }

    for (const std::shared_ptr<MeshGeometry>& geometry : mesh->geometries)
    {
        if (geometry && !geometry->renderResourceId)
        {
            geometry->renderResourceId = scene::GeometryRenderResourceId::make(
                g_nextGeometryRenderResourceId.fetch_add(1, std::memory_order_relaxed), 0);
        }
        if (geometry)
            EnsureMaterialRenderResourceId(geometry->material);
    }
}

} // namespace

void SceneResources::registerMeshInstanceEntity(
    ecs::Entity entity, const std::shared_ptr<MeshInfo>& mesh, bool /*skinned*/)
{
    if (!ecs::isValid(entity) || !mesh)
        return;

    EnsureMeshRenderResourceIds(mesh);
    EnsureMeshRenderResourceIds(mesh->skinPrototype);

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

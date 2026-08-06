#pragma once

// Engine-internal GPU keys for MeshInfo / MeshGeometry / Material.
// Applications must not include this. Use MeshHandle, MaterialHandle,
// MeshInstanceComponent, SceneMeshEdit (entity), SceneSpawn, SceneTransform instead.

#include <scene/SceneTypes.h>

namespace caustica::scene::internal
{

struct RenderResourceAccess
{
    [[nodiscard]] static MeshRenderResourceId meshId(const MeshInfo* mesh)
    {
        return mesh ? mesh->m_renderResourceId : MeshRenderResourceId{};
    }

    [[nodiscard]] static MeshRenderResourceId& meshId(MeshInfo& mesh)
    {
        return mesh.m_renderResourceId;
    }

    [[nodiscard]] static GeometryRenderResourceId geometryId(const MeshGeometry* geometry)
    {
        return geometry ? geometry->m_renderResourceId : GeometryRenderResourceId{};
    }

    [[nodiscard]] static GeometryRenderResourceId& geometryId(MeshGeometry& geometry)
    {
        return geometry.m_renderResourceId;
    }

    [[nodiscard]] static MaterialRenderResourceId materialId(const Material* material)
    {
        return material ? material->m_renderResourceId : MaterialRenderResourceId{};
    }

    [[nodiscard]] static MaterialRenderResourceId& materialId(Material& material)
    {
        return material.m_renderResourceId;
    }
};

} // namespace caustica::scene::internal

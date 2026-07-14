#include <scene/SceneMeshAccess.h>

#include <scene/SceneEcs.h>

namespace caustica::scene
{

SceneContentFlags GetMeshContentFlags(const MeshInfo& mesh)
{
    SceneContentFlags flags = SceneContentFlags::None;

    for (const auto& geometry : mesh.geometries)
    {
        if (!geometry->material)
            continue;

        switch (geometry->material->domain) // NOLINT(clang-diagnostic-switch-enum)
        {
        case MaterialDomain::Opaque:
            flags |= SceneContentFlags::OpaqueMeshes;
            break;
        case MaterialDomain::AlphaTested:
            flags |= SceneContentFlags::AlphaTestedMeshes;
            break;
        default:
            flags |= SceneContentFlags::BlendedMeshes;
            break;
        }
    }

    return flags;
}

dm::box3 GetMeshLocalBounds(const MeshInfo& mesh)
{
    return mesh.objectSpaceBounds;
}

bool SetMeshProperty(MeshInfo& mesh, const std::string& propName, const dm::float4& value)
{
    if (mesh.geometries.size() == 1 && mesh.geometries[0]->material)
        return mesh.geometries[0]->material->SetProperty(propName, value);

    return false;
}

void InitializeMeshInstanceComponent(MeshInstanceComponent& component, const std::shared_ptr<MeshInfo>& mesh)
{
    component.mesh = mesh;
    component.instanceIndex = -1;
    component.geometryInstanceIndex = -1;
    component.proxiedAnalyticLight = ecs::NullEntity;
    component.perGeometryLightSamplerLinks.clear();
    if (mesh)
        component.perGeometryLightSamplerLinks.resize(mesh->geometries.size(), LightSamplerLink{ -1, -1 });
}

const MeshInstanceComponent* TryGetMeshInstance(const ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<MeshInstanceComponent>(entity);
}

std::shared_ptr<MeshInfo> GetMeshAsset(const ecs::World& world, ecs::Entity entity)
{
    const auto* component = TryGetMeshInstance(world, entity);
    return component ? component->mesh : nullptr;
}

bool HasSkinnedMesh(const ecs::World& world, ecs::Entity entity)
{
    return world.has<SkinnedMeshComponent>(entity);
}

const SkinnedMeshComponent* TryGetSkinnedMesh(const ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<SkinnedMeshComponent>(entity);
}

SkinnedMeshComponent* TryGetSkinnedMesh(ecs::World& world, ecs::Entity entity)
{
    return world.tryGet<SkinnedMeshComponent>(entity);
}

std::shared_ptr<MeshInfo> CreateSkinnedMeshFromPrototype(
    SceneTypeFactory& factory, const std::shared_ptr<MeshInfo>& prototypeMesh)
{
    auto skinnedMesh = factory.CreateMesh();
    skinnedMesh->skinPrototype = prototypeMesh;
    skinnedMesh->name = prototypeMesh->name;
    skinnedMesh->objectSpaceBounds = prototypeMesh->objectSpaceBounds;
    skinnedMesh->indexOffset = prototypeMesh->indexOffset;
    skinnedMesh->vertexOffset = 0;
    skinnedMesh->totalVertices = prototypeMesh->totalVertices;
    skinnedMesh->totalIndices = prototypeMesh->totalIndices;
    skinnedMesh->geometries.reserve(prototypeMesh->geometries.size());

    for (const auto& geometry : prototypeMesh->geometries)
    {
        auto newGeometry = factory.CreateMeshGeometry();
        *newGeometry = *geometry;
        skinnedMesh->geometries.push_back(std::move(newGeometry));
    }

    return skinnedMesh;
}

} // namespace caustica::scene

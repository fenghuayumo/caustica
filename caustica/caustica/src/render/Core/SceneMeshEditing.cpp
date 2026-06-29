#include <render/Core/SceneMeshEditing.h>

#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <scene/SceneObjects.h>
#include <scene/SceneTypes.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace caustica
{

namespace
{

using dm::affine3;
using dm::box3;
using dm::float3;

struct UniquePositionMap
{
    std::vector<float3> uniquePositions;
    std::vector<uint32_t> renderToUnique;
};

std::array<uint32_t, 3> PositionKey(const float3& p)
{
    std::array<uint32_t, 3> key{};
    std::memcpy(&key[0], &p.x, sizeof(uint32_t));
    std::memcpy(&key[1], &p.y, sizeof(uint32_t));
    std::memcpy(&key[2], &p.z, sizeof(uint32_t));
    return key;
}

UniquePositionMap BuildUniquePositionMap(
    const std::vector<float3>& renderVertices,
    const std::vector<uint32_t>* sourcePositionIndices = nullptr)
{
    if (sourcePositionIndices && sourcePositionIndices->size() == renderVertices.size())
    {
        UniquePositionMap result;
        result.renderToUnique.resize(renderVertices.size());

        std::vector<size_t> renderOrder;
        renderOrder.reserve(renderVertices.size());
        for (size_t i = 0; i < renderVertices.size(); ++i)
            renderOrder.push_back(i);

        std::sort(renderOrder.begin(), renderOrder.end(),
            [&](size_t a, size_t b)
            {
                const uint32_t sourceA = (*sourcePositionIndices)[a];
                const uint32_t sourceB = (*sourcePositionIndices)[b];
                return sourceA == sourceB ? a < b : sourceA < sourceB;
            });

        std::unordered_map<uint32_t, uint32_t> uniqueLookup;
        uniqueLookup.reserve(renderVertices.size());

        for (size_t renderIndex : renderOrder)
        {
            const uint32_t sourceIndex = (*sourcePositionIndices)[renderIndex];
            auto found = uniqueLookup.find(sourceIndex);
            if (found == uniqueLookup.end())
            {
                const uint32_t uniqueIndex = static_cast<uint32_t>(result.uniquePositions.size());
                uniqueLookup.emplace(sourceIndex, uniqueIndex);
                result.uniquePositions.push_back(renderVertices[renderIndex]);
                result.renderToUnique[renderIndex] = uniqueIndex;
            }
            else
            {
                result.renderToUnique[renderIndex] = found->second;
            }
        }

        return result;
    }

    struct KeyHash
    {
        size_t operator()(const std::array<uint32_t, 3>& key) const noexcept
        {
            size_t h = std::hash<uint32_t>{}(key[0]);
            h ^= std::hash<uint32_t>{}(key[1]) + 0x9e3779b9u + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(key[2]) + 0x9e3779b9u + (h << 6) + (h >> 2);
            return h;
        }
    };

    UniquePositionMap result;
    result.renderToUnique.reserve(renderVertices.size());

    std::unordered_map<std::array<uint32_t, 3>, uint32_t, KeyHash> uniqueLookup;
    uniqueLookup.reserve(renderVertices.size());

    for (const float3& vertex : renderVertices)
    {
        const auto key = PositionKey(vertex);
        auto found = uniqueLookup.find(key);
        if (found == uniqueLookup.end())
        {
            const uint32_t uniqueIndex = static_cast<uint32_t>(result.uniquePositions.size());
            uniqueLookup.emplace(key, uniqueIndex);
            result.uniquePositions.push_back(vertex);
            result.renderToUnique.push_back(uniqueIndex);
        }
        else
        {
            result.renderToUnique.push_back(found->second);
        }
    }

    return result;
}

std::vector<float3> GetMeshRenderVertices(const std::shared_ptr<MeshInfo>& mesh, const char* caller)
{
    if (!mesh)
        throw std::runtime_error(std::string(caller) + ": mesh is null");
    if (!mesh->buffers)
        throw std::runtime_error(std::string(caller) + ": mesh has no buffer group");
    if (mesh->totalVertices == 0)
        return {};

    const auto& positions = mesh->buffers->positionData;
    const size_t begin = size_t(mesh->vertexOffset);
    const size_t end = begin + size_t(mesh->totalVertices);
    if (positions.size() < end)
        throw std::runtime_error(std::string(caller) + ": CPU vertex cache is unavailable; reload the scene with the Python deformation build");

    return std::vector<float3>(positions.begin() + begin, positions.begin() + end);
}

const std::vector<uint32_t>* GetMeshSourcePositionIndices(
    const std::shared_ptr<MeshInfo>& mesh,
    size_t renderVertexCount)
{
    if (!mesh || mesh->DeformationSourcePositionIndices.size() != renderVertexCount)
        return nullptr;

    return &mesh->DeformationSourcePositionIndices;
}

std::shared_ptr<MeshInstance> GetMeshFromEntity(
    const scene::SceneEntityWorld& entityWorld,
    ecs::Entity entity,
    const char* caller)
{
    if (!ecs::isValid(entity))
        throw std::runtime_error(std::string(caller) + ": entity is invalid");

    const auto* meshComponent = entityWorld.world().get<scene::MeshInstanceComponent>(entity);
    if (!meshComponent || !meshComponent->instance || !meshComponent->instance->GetMesh())
        throw std::runtime_error(std::string(caller) + ": entity is not a mesh instance");

    return meshComponent->instance;
}

ecs::Entity FindUniqueMeshInstanceEntity(
    const std::shared_ptr<Scene>& scene,
    const std::shared_ptr<MeshInfo>& mesh,
    const char* caller)
{
    if (!mesh)
        throw std::runtime_error(std::string(caller) + ": mesh is null");
    if (!scene)
        throw std::runtime_error(std::string(caller) + ": no scene is loaded");

    ecs::Entity result = ecs::NullEntity;
    size_t instanceCount = 0;

    for (const auto& instance : scene->GetMeshInstances())
    {
        if (!instance || instance->GetMesh() != mesh)
            continue;

        if (ecs::isValid(instance->ownerEntity))
        {
            result = instance->ownerEntity;
            ++instanceCount;
        }
    }

    if (instanceCount == 0)
        throw std::runtime_error(std::string(caller) + ": mesh has no scene instance");
    if (instanceCount > 1)
        throw std::runtime_error(std::string(caller) + ": mesh has multiple scene instances; pass an entity instead");

    return result;
}

dm::affine3 GetEntityTransformFloat(const scene::SceneEntityWorld& entityWorld, ecs::Entity entity)
{
    if (const auto* global = entityWorld.world().get<scene::GlobalTransformComponent>(entity))
        return global->transformFloat;
    return dm::affine3::identity();
}

float3 NormalizeOrFallback(const float3& v, const float3& fallback)
{
    const float len2 = dot(v, v);
    if (len2 <= 1e-20f || !std::isfinite(len2))
        return fallback;
    return v * (1.0f / std::sqrt(len2));
}

void RecomputeMeshNormalsFromPositions(const std::shared_ptr<MeshInfo>& mesh)
{
    auto& buffers = *mesh->buffers;
    const size_t vertexBegin = size_t(mesh->vertexOffset);
    const size_t vertexEnd = vertexBegin + size_t(mesh->totalVertices);

    if (buffers.normalData.size() < vertexEnd || buffers.indexData.empty())
        return;

    std::vector<float3> renderVertices(buffers.positionData.begin() + vertexBegin, buffers.positionData.begin() + vertexEnd);
    UniquePositionMap uniqueMap = BuildUniquePositionMap(
        renderVertices,
        GetMeshSourcePositionIndices(mesh, renderVertices.size()));

    std::vector<float3> accumulated(uniqueMap.uniquePositions.size(), float3(0.0f));

    for (const auto& geometry : mesh->geometries)
    {
        if (!geometry || geometry->type != MeshGeometryPrimitiveType::Triangles)
            continue;

        const size_t indexBegin = size_t(mesh->indexOffset) + size_t(geometry->indexOffsetInMesh);
        const size_t indexEnd = indexBegin + size_t(geometry->numIndices);
        if (buffers.indexData.size() < indexEnd)
            continue;

        for (size_t i = indexBegin; i + 2 < indexEnd; i += 3)
        {
            uint32_t local[3] = {
                buffers.indexData[i + 0],
                buffers.indexData[i + 1],
                buffers.indexData[i + 2]
            };

            const uint32_t maxIndex = std::max(local[0], std::max(local[1], local[2]));
            if (maxIndex < geometry->numVertices)
            {
                local[0] += geometry->vertexOffsetInMesh;
                local[1] += geometry->vertexOffsetInMesh;
                local[2] += geometry->vertexOffsetInMesh;
            }

            if (local[0] >= mesh->totalVertices || local[1] >= mesh->totalVertices || local[2] >= mesh->totalVertices)
                continue;

            const float3& p0 = buffers.positionData[vertexBegin + local[0]];
            const float3& p1 = buffers.positionData[vertexBegin + local[1]];
            const float3& p2 = buffers.positionData[vertexBegin + local[2]];
            const float3 faceNormal = cross(p1 - p0, p2 - p0);
            if (!std::isfinite(dot(faceNormal, faceNormal)))
                continue;

            accumulated[uniqueMap.renderToUnique[local[0]]] += faceNormal;
            accumulated[uniqueMap.renderToUnique[local[1]]] += faceNormal;
            accumulated[uniqueMap.renderToUnique[local[2]]] += faceNormal;
        }
    }

    for (uint32_t i = 0; i < mesh->totalVertices; ++i)
    {
        const float3 normal = NormalizeOrFallback(accumulated[uniqueMap.renderToUnique[i]], float3(0.0f, 1.0f, 0.0f));
        buffers.normalData[vertexBegin + i] = vectorToSnorm8(normal);
    }
}

void UpdateMeshBoundsFromPositions(const std::shared_ptr<MeshInfo>& mesh)
{
    auto& buffers = *mesh->buffers;
    const size_t vertexBegin = size_t(mesh->vertexOffset);
    const size_t vertexEnd = vertexBegin + size_t(mesh->totalVertices);
    if (buffers.positionData.size() < vertexEnd)
        return;

    mesh->objectSpaceBounds = box3::empty();
    for (const auto& geometry : mesh->geometries)
    {
        if (!geometry)
            continue;

        box3 bounds = box3::empty();
        const size_t geomBegin = vertexBegin + size_t(geometry->vertexOffsetInMesh);
        const size_t geomEnd = std::min(vertexEnd, geomBegin + size_t(geometry->numVertices));
        for (size_t i = geomBegin; i < geomEnd; ++i)
            bounds |= buffers.positionData[i];

        geometry->objectSpaceBounds = bounds;
        mesh->objectSpaceBounds |= bounds;
    }
}

bool BufferRangeContainsBytes(const nvrhi::BufferRange& range, uint64_t relativeOffset, uint64_t byteSize)
{
    return range.byteSize != 0 && relativeOffset <= range.byteSize && byteSize <= range.byteSize - relativeOffset;
}

nvrhi::ResourceStates GetMeshVertexBufferReadyState(const nvrhi::BufferDesc& desc)
{
    nvrhi::ResourceStates state = nvrhi::ResourceStates::VertexBuffer | nvrhi::ResourceStates::ShaderResource;
    if (desc.isAccelStructBuildInput)
        state = state | nvrhi::ResourceStates::AccelStructBuildInput;
    return state;
}

bool UploadMeshDeformationToGpu(
    nvrhi::IDevice* device,
    const std::shared_ptr<MeshInfo>& mesh,
    size_t renderVertexCount,
    const std::vector<float3>* previousRenderVertices,
    bool uploadNormals)
{
    if (!device || !mesh || !mesh->buffers)
        return false;

    auto& buffers = *mesh->buffers;
    if (!buffers.vertexBuffer)
        return false;

    const size_t begin = size_t(mesh->vertexOffset);
    const size_t end = begin + renderVertexCount;
    if (buffers.positionData.size() < end)
        return false;

    const nvrhi::BufferRange& positionRange = buffers.getVertexBufferRange(VertexAttribute::Position);
    const uint64_t positionOffset = uint64_t(begin) * sizeof(float3);
    const uint64_t positionBytes = uint64_t(renderVertexCount) * sizeof(float3);
    if (!BufferRangeContainsBytes(positionRange, positionOffset, positionBytes))
        return false;

    nvrhi::CommandListHandle commandList = device->createCommandList();
    commandList->open();
    commandList->writeBuffer(
        buffers.vertexBuffer,
        buffers.positionData.data() + begin,
        positionBytes,
        positionRange.byteOffset + positionOffset);

    if (previousRenderVertices && previousRenderVertices->size() == renderVertexCount)
    {
        const nvrhi::BufferRange& prevPositionRange = buffers.getVertexBufferRange(VertexAttribute::PrevPosition);
        if (BufferRangeContainsBytes(prevPositionRange, positionOffset, positionBytes))
        {
            commandList->writeBuffer(
                buffers.vertexBuffer,
                previousRenderVertices->data(),
                positionBytes,
                prevPositionRange.byteOffset + positionOffset);
        }
    }

    if (uploadNormals && buffers.normalData.size() >= end)
    {
        const nvrhi::BufferRange& normalRange = buffers.getVertexBufferRange(VertexAttribute::Normal);
        const uint64_t normalOffset = uint64_t(begin) * sizeof(uint32_t);
        const uint64_t normalBytes = uint64_t(renderVertexCount) * sizeof(uint32_t);
        if (BufferRangeContainsBytes(normalRange, normalOffset, normalBytes))
        {
            commandList->writeBuffer(
                buffers.vertexBuffer,
                buffers.normalData.data() + begin,
                normalBytes,
                normalRange.byteOffset + normalOffset);
        }
    }

    commandList->setBufferState(buffers.vertexBuffer, GetMeshVertexBufferReadyState(buffers.vertexBuffer->getDesc()));
    commandList->close();
    device->executeCommandList(commandList);
    return true;
}

void RebuildSceneMeshBuffersIfNeeded(const std::shared_ptr<MeshInfo>& mesh, const SetSceneMeshVerticesParams& params)
{
    if (!params.device)
        return;

    params.device->waitForIdle();

    auto& buffers = *mesh->buffers;
    if (buffers.vertexBuffer)
    {
        buffers.vertexBuffer = nullptr;
        buffers.vertexBufferDescriptor.reset();
        buffers.vertexBufferRanges.fill(nvrhi::BufferRange());
    }

    if (params.scene)
        params.scene->FinishedLoading(params.frameIndex);
}

} // namespace

std::vector<dm::float3> GetMeshVertices(const std::shared_ptr<MeshInfo>& mesh)
{
    std::vector<float3> renderVertices = GetMeshRenderVertices(mesh, "GetMeshVertices");
    return BuildUniquePositionMap(
        renderVertices,
        GetMeshSourcePositionIndices(mesh, renderVertices.size())).uniquePositions;
}

std::vector<dm::float3> GetMeshVerticesWorld(
    const std::shared_ptr<Scene>& scene,
    const std::shared_ptr<MeshInfo>& mesh,
    uint32_t frameIndex)
{
    auto entity = FindUniqueMeshInstanceEntity(scene, mesh, "GetMeshVerticesWorld");
    return GetMeshVerticesWorld(scene, entity, frameIndex);
}

std::vector<dm::float3> GetMeshVerticesWorld(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity entity,
    uint32_t frameIndex)
{
    if (!scene)
        throw std::runtime_error("GetMeshVerticesWorld: no scene is loaded");

    scene->RefreshSceneWorld(frameIndex);

    const scene::SceneEntityWorld* entityWorld = scene->GetEntityWorld();
    if (!entityWorld)
        throw std::runtime_error("GetMeshVerticesWorld: scene has no entity world");

    auto meshInstance = GetMeshFromEntity(*entityWorld, entity, "GetMeshVerticesWorld");
    auto mesh = meshInstance->GetMesh();
    std::vector<float3> vertices = GetMeshVertices(mesh);

    const affine3 localToWorld = GetEntityTransformFloat(*entityWorld, entity);
    for (float3& vertex : vertices)
        vertex = localToWorld.transformPoint(vertex);

    return vertices;
}

void SetMeshVertices(
    const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<dm::float3>& vertices,
    const SetSceneMeshVerticesParams& params)
{
    if (!mesh)
        throw std::runtime_error("SetMeshVertices: mesh is null");
    if (!mesh->buffers)
        throw std::runtime_error("SetMeshVertices: mesh has no buffer group");

    std::vector<float3> renderVertices = GetMeshRenderVertices(mesh, "SetMeshVertices");
    UniquePositionMap uniqueMap = BuildUniquePositionMap(
        renderVertices,
        GetMeshSourcePositionIndices(mesh, renderVertices.size()));
    if (vertices.size() != uniqueMap.uniquePositions.size())
    {
        throw std::runtime_error(
            "SetMeshVertices: vertex count must match get_mesh_vertices(...) length");
    }

    for (size_t i = 0; i < renderVertices.size(); ++i)
        renderVertices[i] = vertices[uniqueMap.renderToUnique[i]];

    auto& buffers = *mesh->buffers;
    const size_t begin = size_t(mesh->vertexOffset);
    const size_t end = begin + renderVertices.size();
    if (buffers.positionData.size() < end)
        throw std::runtime_error("SetMeshVertices: CPU vertex cache is unavailable; reload the scene with the Python deformation build");

    const std::vector<float3> previousRenderVertices(buffers.positionData.begin() + begin, buffers.positionData.begin() + end);
    std::copy(renderVertices.begin(), renderVertices.end(), buffers.positionData.begin() + begin);
    UpdateMeshBoundsFromPositions(mesh);

    if (params.recomputeNormals)
        RecomputeMeshNormalsFromPositions(mesh);

    const bool uploadedToExistingGpuBuffer = UploadMeshDeformationToGpu(
        params.device,
        mesh,
        renderVertices.size(),
        &previousRenderVertices,
        params.recomputeNormals);

    if (!uploadedToExistingGpuBuffer)
        RebuildSceneMeshBuffersIfNeeded(mesh, params);

    if (params.rebuildAccelerationStructure)
    {
        if (params.requestMeshAccelRebuild)
            params.requestMeshAccelRebuild(mesh);
    }
    else if (params.resetAccumulation)
    {
        *params.resetAccumulation = true;
    }
}

void SetMeshVerticesWorld(
    ecs::Entity entity,
    const std::vector<dm::float3>& vertices,
    const SetSceneMeshVerticesParams& params)
{
    if (!params.scene)
        throw std::runtime_error("SetMeshVerticesWorld: no scene is loaded");

    params.scene->RefreshSceneWorld(params.frameIndex);

    const scene::SceneEntityWorld* entityWorld = params.scene->GetEntityWorld();
    if (!entityWorld)
        throw std::runtime_error("SetMeshVerticesWorld: scene has no entity world");

    auto meshInstance = GetMeshFromEntity(*entityWorld, entity, "SetMeshVerticesWorld");
    auto mesh = meshInstance->GetMesh();
    const affine3 worldToLocal = affine3(inverse(dm::daffine3(GetEntityTransformFloat(*entityWorld, entity))));

    std::vector<float3> objectVertices;
    objectVertices.reserve(vertices.size());
    for (const float3& vertex : vertices)
    {
        const float3 objectVertex = worldToLocal.transformPoint(vertex);
        if (!dm::all(dm::isfinite(objectVertex)))
            throw std::runtime_error("SetMeshVerticesWorld: world-to-object transform produced a non-finite vertex");
        objectVertices.push_back(objectVertex);
    }

    SetMeshVertices(mesh, objectVertices, params);
}

void SetMeshVerticesWorld(
    const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<dm::float3>& vertices,
    const SetSceneMeshVerticesParams& params)
{
    auto entity = FindUniqueMeshInstanceEntity(params.scene, mesh, "SetMeshVerticesWorld");
    SetMeshVerticesWorld(entity, vertices, params);
}

} // namespace caustica

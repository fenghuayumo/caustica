#include <engine/SceneMeshEditing.h>
#include <render/SceneGpuResources.h>

#include <core/ThreadContext.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <scene/SceneObjects.h>
#include <scene/SceneTypes.h>

// Logic / game thread only (editor, Python deform, animation geometry sequences).
// Do not call from the dedicated render thread — mutate ECS then Extract.

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

std::shared_ptr<MeshInfo> GetMeshInfoFromEntity(
    const scene::SceneEntityWorld& entityWorld,
    ecs::Entity entity,
    const char* caller)
{
    if (!ecs::isValid(entity))
        throw std::runtime_error(std::string(caller) + ": entity is invalid");

    const auto* meshComponent = entityWorld.world().get<scene::MeshInstanceComponent>(entity);
    if (!meshComponent || !meshComponent->mesh)
        throw std::runtime_error(std::string(caller) + ": entity is not a mesh instance");

    return meshComponent->mesh;
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

    for (const ecs::Entity instanceEntity : scene->getMeshInstances())
    {
        if (!scene->getEntityWorld())
            continue;

        const auto* meshComp = scene->getEntityWorld()->world().get<scene::MeshInstanceComponent>(instanceEntity);
        if (!meshComp || meshComp->mesh != mesh)
            continue;

        result = instanceEntity;
        ++instanceCount;
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

bool UploadMeshDeformationToGpu(
    caustica::rhi::Device* /*device*/,
    render::SceneGpuResources* gpuResources,
    const std::shared_ptr<MeshInfo>& mesh,
    size_t renderVertexCount,
    const std::vector<float3>* previousRenderVertices,
    bool uploadNormals)
{
    if (!gpuResources || !mesh || !mesh->buffers)
        return false;

    auto& buffers = *mesh->buffers;
    const size_t begin = size_t(mesh->vertexOffset);
    const size_t end = begin + renderVertexCount;
    if (buffers.positionData.size() < end)
        return false;

    render::MeshGpuUploadCommand command;
    command.meshId = mesh->renderResourceId;
    command.vertexOffset = mesh->vertexOffset;
    command.positions.assign(buffers.positionData.begin() + begin, buffers.positionData.begin() + end);

    if (previousRenderVertices && previousRenderVertices->size() == renderVertexCount)
        command.previousPositions = *previousRenderVertices;

    if (uploadNormals && buffers.normalData.size() >= end)
        command.normals.assign(buffers.normalData.begin() + begin, buffers.normalData.begin() + end);

    gpuResources->enqueueMeshUpload(std::move(command));
    return true;
}

// Copy current Position → PrevPosition so held keyframes report zero object motion.
bool SyncMeshPrevPositionFromCurrent(
    caustica::rhi::Device* /*device*/,
    render::SceneGpuResources* gpuResources,
    const std::shared_ptr<MeshInfo>& mesh)
{
    if (!gpuResources || !mesh || !mesh->buffers)
        return false;

    auto& buffers = *mesh->buffers;
    const size_t begin = size_t(mesh->vertexOffset);
    const size_t count = size_t(mesh->totalVertices);
    const size_t end = begin + count;
    if (count == 0 || buffers.positionData.size() < end)
        return false;

    render::MeshGpuUploadCommand command;
    command.meshId = mesh->renderResourceId;
    command.vertexOffset = mesh->vertexOffset;
    command.previousPositions.assign(
        buffers.positionData.begin() + begin,
        buffers.positionData.begin() + end);
    gpuResources->enqueueMeshUpload(std::move(command));
    return true;
}

void RebuildSceneMeshBuffersIfNeeded(const std::shared_ptr<MeshInfo>& mesh, const SetSceneMeshVerticesParams& params)
{
    if (!params.device)
        return;

    if (params.gpuResources)
    {
        render::MeshGpuUploadCommand command;
        command.meshId = mesh->renderResourceId;
        command.recreateVertexBuffer = true;
        params.gpuResources->enqueueMeshUpload(std::move(command));
    }
}

} // namespace

std::vector<dm::float3> getMeshVertices(const std::shared_ptr<MeshInfo>& mesh)
{
    assertLogicThread();

    std::vector<float3> renderVertices = GetMeshRenderVertices(mesh, "getMeshVertices");
    return BuildUniquePositionMap(
        renderVertices,
        GetMeshSourcePositionIndices(mesh, renderVertices.size())).uniquePositions;
}

std::vector<dm::float3> getMeshVerticesWorld(
    const std::shared_ptr<Scene>& scene,
    const std::shared_ptr<MeshInfo>& mesh,
    uint32_t frameIndex)
{
    assertLogicThread();

    auto entity = FindUniqueMeshInstanceEntity(scene, mesh, "getMeshVerticesWorld");
    return getMeshVerticesWorld(scene, entity, frameIndex);
}

std::vector<dm::float3> getMeshVerticesWorld(
    const std::shared_ptr<Scene>& scene,
    ecs::Entity entity,
    uint32_t frameIndex)
{
    assertLogicThread();

    if (!scene)
        throw std::runtime_error("getMeshVerticesWorld: no scene is loaded");

    scene->refreshEntityWorldForFrame(frameIndex);

    const scene::SceneEntityWorld* entityWorld = scene->getEntityWorld();
    if (!entityWorld)
        throw std::runtime_error("getMeshVerticesWorld: scene has no entity world");

    auto mesh = GetMeshInfoFromEntity(*entityWorld, entity, "getMeshVerticesWorld");
    std::vector<float3> vertices = getMeshVertices(mesh);

    const affine3 localToWorld = GetEntityTransformFloat(*entityWorld, entity);
    for (float3& vertex : vertices)
        vertex = localToWorld.transformPoint(vertex);

    return vertices;
}

void setMeshVertices(
    const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<dm::float3>& vertices,
    const SetSceneMeshVerticesParams& params)
{
    assertLogicThread();

    if (!mesh)
        throw std::runtime_error("setMeshVertices: mesh is null");
    if (!mesh->buffers)
        throw std::runtime_error("setMeshVertices: mesh has no buffer group");

    std::vector<float3> renderVertices = GetMeshRenderVertices(mesh, "setMeshVertices");
    UniquePositionMap uniqueMap = BuildUniquePositionMap(
        renderVertices,
        GetMeshSourcePositionIndices(mesh, renderVertices.size()));
    if (vertices.size() != uniqueMap.uniquePositions.size())
    {
        throw std::runtime_error(
            "setMeshVertices: vertex count must match get_mesh_vertices(...) length");
    }

    for (size_t i = 0; i < renderVertices.size(); ++i)
        renderVertices[i] = vertices[uniqueMap.renderToUnique[i]];

    auto& buffers = *mesh->buffers;
    const size_t begin = size_t(mesh->vertexOffset);
    const size_t end = begin + renderVertices.size();
    if (buffers.positionData.size() < end)
        throw std::runtime_error("setMeshVertices: CPU vertex cache is unavailable; reload the scene with the Python deformation build");

    const std::vector<float3> previousRenderVertices(buffers.positionData.begin() + begin, buffers.positionData.begin() + end);
    std::copy(renderVertices.begin(), renderVertices.end(), buffers.positionData.begin() + begin);
    UpdateMeshBoundsFromPositions(mesh);

    if (params.recomputeNormals)
        RecomputeMeshNormalsFromPositions(mesh);

    const bool uploadedToExistingGpuBuffer = UploadMeshDeformationToGpu(
        params.device,
        params.gpuResources,
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

void setMeshVerticesWorld(
    ecs::Entity entity,
    const std::vector<dm::float3>& vertices,
    const SetSceneMeshVerticesParams& params)
{
    assertLogicThread();

    if (!params.scene)
        throw std::runtime_error("setMeshVerticesWorld: no scene is loaded");

    params.scene->refreshEntityWorldForFrame(params.frameIndex);

    const scene::SceneEntityWorld* entityWorld = params.scene->getEntityWorld();
    if (!entityWorld)
        throw std::runtime_error("setMeshVerticesWorld: scene has no entity world");

    auto mesh = GetMeshInfoFromEntity(*entityWorld, entity, "setMeshVerticesWorld");
    const affine3 worldToLocal = affine3(inverse(dm::daffine3(GetEntityTransformFloat(*entityWorld, entity))));

    std::vector<float3> objectVertices;
    objectVertices.reserve(vertices.size());
    for (const float3& vertex : vertices)
    {
        const float3 objectVertex = worldToLocal.transformPoint(vertex);
        if (!dm::all(dm::isfinite(objectVertex)))
            throw std::runtime_error("setMeshVerticesWorld: world-to-object transform produced a non-finite vertex");
        objectVertices.push_back(objectVertex);
    }

    setMeshVertices(mesh, objectVertices, params);
}

void setMeshVerticesWorld(
    const std::shared_ptr<MeshInfo>& mesh,
    const std::vector<dm::float3>& vertices,
    const SetSceneMeshVerticesParams& params)
{
    assertLogicThread();

    auto entity = FindUniqueMeshInstanceEntity(params.scene, mesh, "setMeshVerticesWorld");
    setMeshVerticesWorld(entity, vertices, params);
}

void setMeshPositionsDirect(
    const std::shared_ptr<MeshInfo>& mesh,
    const dm::float3* positions,
    size_t count,
    const SetSceneMeshVerticesParams& params)
{
    assertLogicThread();

    if (!mesh)
        throw std::runtime_error("setMeshPositionsDirect: mesh is null");
    if (!mesh->buffers)
        throw std::runtime_error("setMeshPositionsDirect: mesh has no buffer group");
    if (!positions)
        throw std::runtime_error("setMeshPositionsDirect: positions is null");
    if (count != size_t(mesh->totalVertices))
        throw std::runtime_error("setMeshPositionsDirect: vertex count mismatch");

    auto& buffers = *mesh->buffers;
    const size_t begin = size_t(mesh->vertexOffset);
    const size_t end = begin + count;
    if (buffers.positionData.size() < end)
        throw std::runtime_error("setMeshPositionsDirect: CPU vertex cache is unavailable");

    // capture previous pose before overwrite (used for motion vectors).
    const std::vector<float3> previousRenderVertices(
        buffers.positionData.begin() + begin,
        buffers.positionData.begin() + end);

    std::copy(positions, positions + count, buffers.positionData.begin() + begin);
    UpdateMeshBoundsFromPositions(mesh);

    if (params.recomputeNormals)
        RecomputeMeshNormalsFromPositions(mesh);

    // On discontinuous jumps (animation loop wrap), write the new pose into both
    // Position and PrevPosition so temporal filters do not see a huge motion spike.
    const std::vector<float3>* prevForUpload = &previousRenderVertices;
    std::vector<float3> zeroMotionPrev;
    if (params.zeroMotionHistory)
    {
        zeroMotionPrev.assign(positions, positions + count);
        prevForUpload = &zeroMotionPrev;
    }

    const bool uploadedToExistingGpuBuffer = UploadMeshDeformationToGpu(
        params.device,
        params.gpuResources,
        mesh,
        count,
        prevForUpload,
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

bool applyGeometrySequence(
    scene::GeometrySequenceComponent& sequence,
    float timeSeconds,
    const SetSceneMeshVerticesParams& params)
{
    assertLogicThread();

    if (!sequence.mesh || sequence.vertexCount == 0 || sequence.timesSeconds.empty())
        return false;

    const size_t frameCount = sequence.timesSeconds.size();
    if (sequence.positions.size() < frameCount * size_t(sequence.vertexCount) * 3)
        return false;

    auto findFrameIndex = [](const std::vector<float>& times, float t) -> int {
        if (times.empty())
            return -1;
        if (t <= times.front())
            return 0;
        if (t >= times.back())
            return int(times.size()) - 1;
        const auto it = std::upper_bound(times.begin(), times.end(), t);
        return int(it - times.begin()) - 1;
    };

    const int frameA = findFrameIndex(sequence.timesSeconds, timeSeconds);
    if (frameA < 0)
        return false;

    int frameB = frameA;
    float alpha = 0.f;
    if (sequence.interpolateFrames
        && frameA + 1 < int(frameCount)
        && timeSeconds > sequence.timesSeconds[size_t(frameA)])
    {
        frameB = frameA + 1;
        const float t0 = sequence.timesSeconds[size_t(frameA)];
        const float t1 = sequence.timesSeconds[size_t(frameB)];
        const float dt = t1 - t0;
        alpha = (dt > 1e-8f) ? std::clamp((timeSeconds - t0) / dt, 0.f, 1.f) : 0.f;
    }

    if (frameA == sequence.lastAppliedFrameA
        && frameB == sequence.lastAppliedFrameB
        && std::abs(alpha - sequence.lastAppliedAlpha) < 1e-5f)
    {
        // Pose is held across display frames. After a keyframe jump, PrevPosition still
        // encodes the previous keyframe — sync it once so TAA/NRD/DLSS see zero object
        // motion while the mesh is visually static (matches skinning_cs FirstFrame-style
        // Position→PrevPosition copy on idle frames).
        if (sequence.prevPositionsNeedSync && params.device)
        {
            if (SyncMeshPrevPositionFromCurrent(
                    params.device, params.gpuResources, sequence.mesh))
                sequence.prevPositionsNeedSync = false;
        }
        return false;
    }

    // fmod loop wrap: sample index jumps backwards (typically last -> first).
    const bool loopWrapped = sequence.lastAppliedFrameA >= 0 && frameA < sequence.lastAppliedFrameA;

    std::vector<float3> blended(sequence.vertexCount);
    const size_t stride = size_t(sequence.vertexCount) * 3;
    const float* a = sequence.positions.data() + size_t(frameA) * stride;
    const float* b = sequence.positions.data() + size_t(frameB) * stride;
    if (frameA == frameB || alpha <= 0.f)
    {
        for (uint32_t i = 0; i < sequence.vertexCount; ++i)
            blended[i] = float3(a[i * 3 + 0], a[i * 3 + 1], a[i * 3 + 2]);
    }
    else if (alpha >= 1.f)
    {
        for (uint32_t i = 0; i < sequence.vertexCount; ++i)
            blended[i] = float3(b[i * 3 + 0], b[i * 3 + 1], b[i * 3 + 2]);
    }
    else
    {
        const float oneMinus = 1.f - alpha;
        for (uint32_t i = 0; i < sequence.vertexCount; ++i)
        {
            blended[i] = float3(
                a[i * 3 + 0] * oneMinus + b[i * 3 + 0] * alpha,
                a[i * 3 + 1] * oneMinus + b[i * 3 + 1] * alpha,
                a[i * 3 + 2] * oneMinus + b[i * 3 + 2] * alpha);
        }
    }

    SetSceneMeshVerticesParams localParams = params;
    localParams.recomputeNormals = sequence.recomputeNormals;
    if (loopWrapped)
    {
        localParams.zeroMotionHistory = true;
        if (localParams.resetAccumulation)
            *localParams.resetAccumulation = true;
    }
    setMeshPositionsDirect(sequence.mesh, blended.data(), blended.size(), localParams);

    sequence.lastAppliedFrameA = frameA;
    sequence.lastAppliedFrameB = frameB;
    sequence.lastAppliedAlpha = alpha;
    // After a real pose change, PrevPosition holds the previous keyframe for one display
    // frame (correct MV). On subsequent held frames we sync Prev→Current (see above).
    // Loop wrap already wrote Prev=Current, so no deferred sync is needed.
    sequence.prevPositionsNeedSync = !localParams.zeroMotionHistory;
    return true;
}

} // namespace caustica

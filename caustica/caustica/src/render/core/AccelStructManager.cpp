#include <render/core/AccelStructManager.h>

#include <render/core/AccelerationStructureUtil.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <render/SceneGpuResources.h>
#include <scene/SceneRenderData.h>
#include <scene/ResourceTracker.h>
#include <scene/SceneTypes.h>
#include <core/log.h>
#include <rhi/utils.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace caustica
{

AccelStructManager::AccelStructManager(caustica::rhi::Device* device)
    : m_device(device)
{
}

void AccelStructManager::createBlases(caustica::rhi::CommandList* commandList,
                                      std::span<const scene::MeshRenderResourceSnapshot> meshes,
                                      const AccelStructBuildSettings& settings)
{
    uint32_t builtMeshCount = 0;
    uint32_t builtGeometryCount = 0;
    uint64_t builtTriangleCount = 0;
    uint64_t maxMeshTriangleCount = 0;
    std::string maxMeshName;

    for (const scene::MeshRenderResourceSnapshot& mesh : meshes)
    {
        if (mesh.isSkinPrototype || m_sceneGpuResources == nullptr)
            continue;
        const auto meshGpuIt = m_sceneGpuResources->meshRegistry.find(mesh.id);
        if (meshGpuIt == m_sceneGpuResources->meshRegistry.end())
            continue;
        render::MeshGpuRecord& meshGpu = meshGpuIt->second;

        bvh::Config cfg = { .excludeTransmissive = settings.excludeTransmissive };

        caustica::rhi::rt::AccelStructDesc blasDesc =
            bvh::getMeshBlasDesc(cfg, mesh, meshGpu, nullptr, false, m_materialGpuCache);
        assert((int)blasDesc.bottomLevelGeometries.size() < (1 << 12));
        if (blasDesc.bottomLevelGeometries.empty())
            continue;

        uint64_t meshTriangleCount = 0;
        for (const caustica::rhi::rt::GeometryDesc& geometry : blasDesc.bottomLevelGeometries)
        {
            if (geometry.geometryType == caustica::rhi::rt::GeometryType::Triangles)
                meshTriangleCount += geometry.geometryData.triangles.indexCount / 3;
        }
        builtMeshCount++;
        builtGeometryCount += static_cast<uint32_t>(blasDesc.bottomLevelGeometries.size());
        builtTriangleCount += meshTriangleCount;
        if (meshTriangleCount > maxMeshTriangleCount)
        {
            maxMeshTriangleCount = meshTriangleCount;
            maxMeshName = mesh.debugName;
        }

        caustica::rhi::rt::AccelStructHandle as = m_device->createAccelStruct(blasDesc);
        if (!as)
        {
            error("Failed to create BLAS for mesh '%s' (triangles=%llu). Skipping mesh AS.",
                mesh.debugName.c_str(),
                static_cast<unsigned long long>(meshTriangleCount));
            continue;
        }
        caustica::rhi::utils::BuildBottomLevelAccelStruct(commandList, as, blasDesc);
        // Retire the previous BLAS so in-flight frames that still reference the old
        // TLAS keep valid bottom-level handles (double-buffered structure rebuild).
        // Do not touch accelStructOmm here — OpacityMicromapBuilder owns that path.
        if (meshGpu.accelStruct)
            m_retiredBlas.push_back(std::move(meshGpu.accelStruct));
        meshGpu.accelStruct = as;
    }
    m_materialStateRevision = m_materialGpuCache
        ? m_materialGpuCache->materialStateRevision()
        : 0;

    info("Queued BLAS builds: meshes=%u, geometries=%u, triangles=%llu, maxMeshTriangles=%llu, maxMesh='%s'",
        builtMeshCount,
        builtGeometryCount,
        static_cast<unsigned long long>(builtTriangleCount),
        static_cast<unsigned long long>(maxMeshTriangleCount),
        maxMeshName.c_str());
}

void AccelStructManager::createTlas(caustica::rhi::CommandList* commandList, const scene::SceneRenderData& renderData)
{
    (void)commandList;

    caustica::rhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.isTopLevel = true;
    tlasDesc.buildFlags = caustica::rhi::rt::AccelStructBuildFlags::PreferFastTrace;

    m_subInstanceCount = 0;
    for (const scene::MeshInstanceRenderProxy& proxy : renderData.meshInstances)
    {
        m_subInstanceCount += proxy.geometryCount;
    }

    tlasDesc.topLevelMaxInstances = std::max<size_t>(1, renderData.meshInstanceEntities.size());
    assert(tlasDesc.topLevelMaxInstances < (1 << 15));

    caustica::rhi::rt::AccelStructHandle newTlas = m_device->createAccelStruct(tlasDesc);
    if (!newTlas)
    {
        error("Failed to create TLAS for %zu instances.", renderData.meshInstanceEntities.size());
        return;
    }

    caustica::rhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(SubInstanceData) * std::max(1u, m_subInstanceCount);
    bufferDesc.debugName = "Instances";
    bufferDesc.structStride = sizeof(SubInstanceData);
    bufferDesc.canHaveRawViews = false;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.isVertexBuffer = false;
    bufferDesc.initialState = caustica::rhi::ResourceStates::Common;
    bufferDesc.keepInitialState = true;
    caustica::rhi::BufferHandle newSubInstanceBuffer = m_device->createBuffer(bufferDesc);

    // Keep the previous TLAS/sub-instance buffer alive for in-flight frames.
    if (m_topLevelAS)
        m_retiredTopLevelAS.push_back(std::move(m_topLevelAS));
    if (m_subInstanceBuffer)
        m_retiredSubInstanceBuffers.push_back(std::move(m_subInstanceBuffer));

    m_topLevelAS = std::move(newTlas);
    m_subInstanceBuffer = std::move(newSubInstanceBuffer);
    m_subInstanceData.clear();
    m_subInstanceData.assign(m_subInstanceCount, SubInstanceData{});
}

void AccelStructManager::uploadSubInstanceData(caustica::rhi::CommandList* commandList) const
{
    assert(m_subInstanceCount == m_subInstanceData.size());
    if (m_subInstanceData.empty())
        return;
    commandList->writeBuffer(m_subInstanceBuffer, m_subInstanceData.data(),
                             m_subInstanceData.size() * sizeof(SubInstanceData));
}

void AccelStructManager::clearMeshAccelStructs(
    std::span<const scene::MeshRenderResourceSnapshot> meshes)
{
    if (m_sceneGpuResources == nullptr)
        return;
    for (const scene::MeshRenderResourceSnapshot& mesh : meshes)
    {
        const auto meshGpuIt = m_sceneGpuResources->meshRegistry.find(mesh.id);
        if (meshGpuIt == m_sceneGpuResources->meshRegistry.end())
            continue;
        render::MeshGpuRecord& meshGpu = meshGpuIt->second;
        meshGpu.accelStruct = nullptr;
        meshGpu.accelStructOmm = nullptr;
        meshGpu.opacityMicromaps.clear();
        meshGpu.debugData = nullptr;
        meshGpu.debugDataDirty = true;
    }
    std::lock_guard lock(*m_pendingRebuildMutex);
    m_meshesPendingAccelRebuild.clear();
}

void AccelStructManager::requestMeshRebuild(scene::MeshRenderResourceId meshId)
{
    if (!meshId)
        return;

    if (!m_topLevelAS)
        return;

    std::lock_guard lock(*m_pendingRebuildMutex);
    const auto it = std::find(m_meshesPendingAccelRebuild.begin(), m_meshesPendingAccelRebuild.end(), meshId);
    if (it == m_meshesPendingAccelRebuild.end())
        m_meshesPendingAccelRebuild.push_back(meshId);
}

void AccelStructManager::rebuildDirtyMeshes(caustica::rhi::CommandList*            commandList,
                                            const scene::SceneRenderData&   renderData,
                                            const AccelStructBuildSettings& settings,
                                            bool&                           fullRebuildRequested)
{
    if (m_materialGpuCache
        && m_materialStateRevision != m_materialGpuCache->materialStateRevision())
    {
        fullRebuildRequested = true;
        return;
    }

    std::vector<scene::MeshRenderResourceId> dirtyMeshIds;
    {
        std::lock_guard lock(*m_pendingRebuildMutex);
        if (m_meshesPendingAccelRebuild.empty())
            return;
        dirtyMeshIds.swap(m_meshesPendingAccelRebuild);
    }

    for (const scene::MeshRenderResourceId meshId : dirtyMeshIds)
    {
        const auto meshIt = std::find_if(
            renderData.meshSnapshots.begin(),
            renderData.meshSnapshots.end(),
            [meshId](const scene::MeshRenderResourceSnapshot& candidate) {
                return candidate.id == meshId;
            });
        if (meshIt == renderData.meshSnapshots.end() || m_sceneGpuResources == nullptr)
            continue;
        const scene::MeshRenderResourceSnapshot& mesh = *meshIt;
        if (mesh.isSkinPrototype)
            continue;

        const auto meshGpuIt = m_sceneGpuResources->meshRegistry.find(meshId);
        if (meshGpuIt == m_sceneGpuResources->meshRegistry.end())
        {
            fullRebuildRequested = true;
            continue;
        }
        render::MeshGpuRecord& meshGpu = meshGpuIt->second;
        if (!meshGpu.vertexBuffer || !meshGpu.indexBuffer)
        {
            fullRebuildRequested = true;
            continue;
        }

        bvh::Config cfg = { .excludeTransmissive = settings.excludeTransmissive };
        // Geometry-sequence / point-cache meshes keep fixed topology; rebuild like skinned
        // meshes via AllowUpdate + PerformUpdate so we do not allocate a new BLAS every frame.
        caustica::rhi::rt::AccelStructDesc blasDesc =
            bvh::getMeshBlasDesc(cfg, mesh, meshGpu, nullptr, true, m_materialGpuCache);
        assert((int)blasDesc.bottomLevelGeometries.size() < (1 << 12));
        if (blasDesc.bottomLevelGeometries.empty())
            continue;

        const bool canUpdateInPlace = meshGpu.accelStruct
            && (meshGpu.accelStruct->getDesc().buildFlags & caustica::rhi::rt::AccelStructBuildFlags::AllowUpdate) != 0;

        if (canUpdateInPlace)
        {
            blasDesc.buildFlags = caustica::rhi::rt::AccelStructBuildFlags::PreferFastBuild
                | caustica::rhi::rt::AccelStructBuildFlags::PerformUpdate;
            caustica::rhi::utils::BuildBottomLevelAccelStruct(commandList, meshGpu.accelStruct, blasDesc);
        }
        else
        {
            blasDesc.buildFlags = caustica::rhi::rt::AccelStructBuildFlags::PreferFastBuild
                | caustica::rhi::rt::AccelStructBuildFlags::AllowUpdate;
            caustica::rhi::rt::AccelStructHandle as = m_device->createAccelStruct(blasDesc);
            if (!as)
            {
                error("Failed to create updatable BLAS for mesh '%s'. Deferring full rebuild.",
                    mesh.debugName.c_str());
                fullRebuildRequested = true;
                continue;
            }
            caustica::rhi::utils::BuildBottomLevelAccelStruct(commandList, as, blasDesc);
            meshGpu.accelStruct = as;
        }

        // Deformed geometry invalidates an OMM-decorated BLAS even when the standard
        // BLAS was updated in place.
        meshGpu.accelStructOmm = nullptr;
        meshGpu.opacityMicromaps.clear();
        meshGpu.debugData = nullptr;
        meshGpu.geometryDebugData.clear();
        meshGpu.debugDataDirty = true;
    }
}

void AccelStructManager::updateSkinnedBlases(caustica::rhi::CommandList*            commandList,
                                             const scene::SceneRenderData&   renderData,
                                             const AccelStructBuildSettings& settings,
                                             uint32_t                        /*frameIndex*/) const
{
    commandList->beginMarker("Skinned BLAS Updates");
    uint32_t skinnedUpdateCount = 0;
    uint32_t skippedEmptySkinnedUpdateCount = 0;
    uint32_t skippedDuplicateSkinnedUpdateCount = 0;
    std::unordered_set<scene::MeshRenderResourceId, scene::MeshRenderResourceId::Hash>
        preparedSkinnedMeshes;
    std::unordered_set<scene::MeshRenderResourceId, scene::MeshRenderResourceId::Hash>
        updatedSkinnedMeshes;

    for (const scene::SkinnedMeshRenderProxy& proxy : renderData.skinnedMeshes)
    {
        if (!proxy.needsSkinningUpdate || !renderData.findMesh(proxy.meshId) || m_sceneGpuResources == nullptr)
            continue;

        const auto meshGpuIt = m_sceneGpuResources->meshRegistry.find(proxy.meshId);
        if (meshGpuIt == m_sceneGpuResources->meshRegistry.end())
        {
            continue;
        }
        render::MeshGpuRecord& meshGpu = meshGpuIt->second;
        if (!meshGpu.accelStruct || !meshGpu.vertexBuffer)
            continue;
        if (!preparedSkinnedMeshes.insert(proxy.meshId).second)
            continue;

        commandList->setAccelStructState(meshGpu.accelStruct, caustica::rhi::ResourceStates::AccelStructWrite);
        commandList->setBufferState(meshGpu.vertexBuffer,
                                    caustica::rhi::ResourceStates::AccelStructBuildInput);
    }
    commandList->commitBarriers();

    for (const scene::SkinnedMeshRenderProxy& proxy : renderData.skinnedMeshes)
    {
        const scene::MeshRenderResourceSnapshot* mesh = renderData.findMesh(proxy.meshId);
        if (!proxy.needsSkinningUpdate || !mesh || m_sceneGpuResources == nullptr)
            continue;

        const auto meshGpuIt = m_sceneGpuResources->meshRegistry.find(proxy.meshId);
        if (meshGpuIt == m_sceneGpuResources->meshRegistry.end()
            || !meshGpuIt->second.accelStruct)
        {
            continue;
        }
        render::MeshGpuRecord& meshGpu = meshGpuIt->second;
        if (!updatedSkinnedMeshes.insert(proxy.meshId).second)
        {
            skippedDuplicateSkinnedUpdateCount++;
            continue;
        }

        bvh::Config cfg = { .excludeTransmissive = settings.excludeTransmissive };
        caustica::rhi::rt::AccelStructDesc blasDesc =
            bvh::getMeshBlasDesc(cfg, *mesh, meshGpu, nullptr, true, m_materialGpuCache);
        if (blasDesc.bottomLevelGeometries.empty())
        {
            skippedEmptySkinnedUpdateCount++;
            continue;
        }
        caustica::rhi::utils::BuildBottomLevelAccelStruct(commandList, meshGpu.accelStruct, blasDesc);
        // OMM data was baked for the previous vertex positions.
        meshGpu.accelStructOmm = nullptr;
        meshGpu.opacityMicromaps.clear();
        meshGpu.debugData = nullptr;
        meshGpu.geometryDebugData.clear();
        meshGpu.debugDataDirty = true;
        skinnedUpdateCount++;
    }
    if (skippedDuplicateSkinnedUpdateCount > 0)
    {
        static bool duplicateSkinnedBlasWarningShown = false;
        if (!duplicateSkinnedBlasWarningShown)
        {
            warning("Skipped %u duplicate skinned BLAS updates; updated %u unique skinned meshes.",
                skippedDuplicateSkinnedUpdateCount, skinnedUpdateCount);
            duplicateSkinnedBlasWarningShown = true;
        }
    }
    if (skippedEmptySkinnedUpdateCount > 0)
    {
        warning("Skipped %u empty skinned BLAS updates.", skippedEmptySkinnedUpdateCount);
    }
    commandList->endMarker();
}

void AccelStructManager::buildTlas(caustica::rhi::CommandList*            commandList,
                                   const scene::SceneRenderData&   renderData,
                                   const AccelStructBuildSettings& settings,
                                   const OmmAccelStructState&      ommState,
                                   OpacityMicromapBuilder*                       opacityMicromapBuilder) const
{
    std::vector<caustica::rhi::rt::InstanceDesc> instances;

    uint subInstanceCount = 0;
    instances.reserve(renderData.meshInstances.size());

    // One TLAS slot per meshInstances entry so DXR InstanceIndex() matches ECS
    // instanceIndex / instanceBuffer / pick → findEntityByInstanceIndex().
    // Missing mesh or BLAS becomes an empty instance (mask 0) via AllowEmptyInstances.
    for (const scene::MeshInstanceRenderProxy& proxy : renderData.meshInstances)
    {
        assert(proxy.instanceIndex < 0
            || static_cast<size_t>(proxy.instanceIndex) == instances.size());

        // geometryInstanceIndex must be a dense prefix sum after refreshInstanceIndices.
        // Prefer the compacted running count if a stale snapshot slips through.
        if (proxy.meshId
            && subInstanceCount != static_cast<uint32_t>(proxy.geometryInstanceIndex))
        {
            static bool warnedStaleGeometryIndex = false;
            if (!warnedStaleGeometryIndex)
            {
                warning("BuildTLAS: geometryInstanceIndex mismatch (got %d, expected %u); using compacted index.",
                    proxy.geometryInstanceIndex, subInstanceCount);
                warnedStaleGeometryIndex = true;
            }
        }
        const uint32_t compactedGeometryInstanceIndex = subInstanceCount;

        const dm::affine3& transform = proxy.transformFloat;
        const bool finiteTransform =
            dm::all(dm::isfinite(transform.m_linear[0]))
            && dm::all(dm::isfinite(transform.m_linear[1]))
            && dm::all(dm::isfinite(transform.m_linear[2]))
            && dm::all(dm::isfinite(transform.m_translation));
        const bool validTransform =
            finiteTransform && std::abs(dm::determinant(transform.m_linear)) > 1e-12f;

        caustica::rhi::rt::InstanceDesc instanceDesc;
        instanceDesc.instanceID = compactedGeometryInstanceIndex;
        instanceDesc.instanceContributionToHitGroupIndex = compactedGeometryInstanceIndex;
        instanceDesc.flags = caustica::rhi::rt::InstanceFlags::None;
        dm::affineToColumnMajor(
            validTransform ? transform : dm::affine3::identity(),
            instanceDesc.transform);

        const scene::MeshRenderResourceSnapshot* mesh = renderData.findMesh(proxy.meshId);
        if (!mesh)
        {
            instanceDesc.bottomLevelAS = nullptr;
            instanceDesc.instanceMask = 0;
            instances.push_back(instanceDesc);
            continue;
        }

        const uint32_t meshSubInstanceCount = (uint32_t)mesh->geometries.size();
        if (!validTransform)
        {
            static bool warnedInvalidTransform = false;
            if (!warnedInvalidTransform)
            {
                warning("BuildTLAS: non-finite or degenerate instance transform; "
                    "inserting an empty TLAS slot to avoid a D3D12 device removal.");
                warnedInvalidTransform = true;
            }
            instanceDesc.bottomLevelAS = nullptr;
            instanceDesc.instanceMask = 0;
            subInstanceCount += meshSubInstanceCount;
            instances.push_back(instanceDesc);
            continue;
        }

        // Hierarchy-hidden instances keep their slot but contribute no hits.
        if (!proxy.enabled)
        {
            instanceDesc.bottomLevelAS = nullptr;
            instanceDesc.instanceMask = 0;
            subInstanceCount += meshSubInstanceCount;
            instances.push_back(instanceDesc);
            continue;
        }

        const auto meshGpuIt = m_sceneGpuResources
            ? m_sceneGpuResources->meshRegistry.find(proxy.meshId)
            : decltype(m_sceneGpuResources->meshRegistry.find(proxy.meshId)){};
        if (m_sceneGpuResources == nullptr || meshGpuIt == m_sceneGpuResources->meshRegistry.end())
        {
            instanceDesc.bottomLevelAS = nullptr;
            instanceDesc.instanceMask = 0;
            subInstanceCount += meshSubInstanceCount;
            instances.push_back(instanceDesc);
            continue;
        }
        const render::MeshGpuRecord& meshGpu = meshGpuIt->second;

        const bool hasAttachementOMM = opacityMicromapBuilder && meshGpu.accelStructOmm.Get() != nullptr;
        const bool useOmmBLAS = ommState.enabled && hasAttachementOMM && !settings.forceOpaque && !ommState.debugViewEnabled;
        auto* bottomLevelAS = useOmmBLAS ? meshGpu.accelStructOmm.Get() : meshGpu.accelStruct.Get();

        if (bottomLevelAS == nullptr)
        {
            static bool warnedNullBlas = false;
            if (!warnedNullBlas)
            {
                warning("BuildTLAS: one or more mesh instances have null BLAS; "
                    "inserting empty TLAS slots so InstanceIndex stays aligned with ECS.");
                warnedNullBlas = true;
            }
            instanceDesc.bottomLevelAS = nullptr;
            instanceDesc.instanceMask = 0;
            subInstanceCount += meshSubInstanceCount;
            instances.push_back(instanceDesc);
            continue;
        }

        instanceDesc.bottomLevelAS = bottomLevelAS;
        instanceDesc.instanceMask = (ommState.onlyOMMs && !hasAttachementOMM) ? 0 : 1;
        instanceDesc.flags = ommState.force2State ? caustica::rhi::rt::InstanceFlags::ForceOMM2State : caustica::rhi::rt::InstanceFlags::None;
        if (settings.forceOpaque || ommState.debugViewEnabled)
            instanceDesc.flags = (caustica::rhi::rt::InstanceFlags)((uint32_t)instanceDesc.flags | (uint32_t)caustica::rhi::rt::InstanceFlags::ForceOpaque);

        subInstanceCount += meshSubInstanceCount;
        instances.push_back(instanceDesc);
    }
    assert(m_subInstanceCount == subInstanceCount);
    assert(instances.size() == renderData.meshInstances.size());

    if (!m_topLevelAS)
    {
        warning("BuildTLAS: top-level AS is null; skipping.");
        return;
    }

    const size_t tlasMaxInstances = m_topLevelAS->getDesc().topLevelMaxInstances;
    if (instances.size() > tlasMaxInstances)
    {
        error("BuildTLAS: instance count %zu exceeds TLAS capacity %zu; skipping to avoid driver crash. "
            "Request a full acceleration-structure rebuild.",
            instances.size(), tlasMaxInstances);
        return;
    }

    if (m_subInstanceCount != subInstanceCount)
    {
        error("BuildTLAS: sub-instance count mismatch (allocated %u, built %u); skipping.",
            m_subInstanceCount, subInstanceCount);
        return;
    }

    commandList->beginMarker("TLAS update");
    commandList->buildTopLevelAccelStruct(
        m_topLevelAS,
        instances.empty() ? nullptr : instances.data(),
        instances.size(),
        caustica::rhi::rt::AccelStructBuildFlags::AllowEmptyInstances);
    commandList->endMarker();
}

void AccelStructManager::clearRetiredAccelStructs()
{
    m_retiredTopLevelAS.clear();
    m_retiredSubInstanceBuffers.clear();
    m_retiredBlas.clear();
}

void AccelStructManager::releaseGpuResources()
{
    m_topLevelAS = nullptr;
    m_subInstanceBuffer = nullptr;
    m_subInstanceData.clear();
    m_subInstanceCount = 0;
    clearRetiredAccelStructs();
    std::lock_guard lock(*m_pendingRebuildMutex);
    m_meshesPendingAccelRebuild.clear();
}

} // namespace caustica

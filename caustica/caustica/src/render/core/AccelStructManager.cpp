#include <render/core/AccelStructManager.h>

#include <render/core/AccelerationStructureUtil.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <scene/Scene.h>
#include <scene/SceneRenderData.h>
#include <scene/ResourceTracker.h>
#include <core/log.h>
#include <rhi/utils.h>

#include <algorithm>
#include <unordered_set>

namespace caustica
{

AccelStructManager::AccelStructManager(nvrhi::IDevice* device)
    : m_device(device)
{
}

void AccelStructManager::createBlases(nvrhi::ICommandList* commandList,
                                      const Scene&         scene,
                                      const AccelStructBuildSettings& settings)
{
    uint32_t builtMeshCount = 0;
    uint32_t builtGeometryCount = 0;
    uint64_t builtTriangleCount = 0;
    uint64_t maxMeshTriangleCount = 0;
    std::string maxMeshName;

    for (const std::shared_ptr<MeshInfo>& mesh : scene.getMeshes())
    {
        if (mesh->isSkinPrototype)
            continue;

        bvh::Config cfg = { .excludeTransmissive = settings.excludeTransmissive };

        nvrhi::rt::AccelStructDesc blasDesc = bvh::getMeshBlasDesc(cfg, *mesh, nullptr, false);
        assert((int)blasDesc.bottomLevelGeometries.size() < (1 << 12));
        if (blasDesc.bottomLevelGeometries.empty())
            continue;

        uint64_t meshTriangleCount = 0;
        for (const nvrhi::rt::GeometryDesc& geometry : blasDesc.bottomLevelGeometries)
        {
            if (geometry.geometryType == nvrhi::rt::GeometryType::Triangles)
                meshTriangleCount += geometry.geometryData.triangles.indexCount / 3;
        }
        builtMeshCount++;
        builtGeometryCount += static_cast<uint32_t>(blasDesc.bottomLevelGeometries.size());
        builtTriangleCount += meshTriangleCount;
        if (meshTriangleCount > maxMeshTriangleCount)
        {
            maxMeshTriangleCount = meshTriangleCount;
            maxMeshName = mesh->name;
        }

        nvrhi::rt::AccelStructHandle as = m_device->createAccelStruct(blasDesc);
        nvrhi::utils::BuildBottomLevelAccelStruct(commandList, as, blasDesc);
        mesh->accelStruct = as;
    }

    info("Queued BLAS builds: meshes=%u, geometries=%u, triangles=%llu, maxMeshTriangles=%llu, maxMesh='%s'",
        builtMeshCount,
        builtGeometryCount,
        static_cast<unsigned long long>(builtTriangleCount),
        static_cast<unsigned long long>(maxMeshTriangleCount),
        maxMeshName.c_str());
}

void AccelStructManager::createTlas(nvrhi::ICommandList* commandList, const Scene& scene)
{
    (void)commandList;

    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.isTopLevel = true;
    tlasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;

    m_subInstanceCount = 0;
    const scene::SceneRenderData& renderData = scene.getRenderData();
    for (const scene::MeshInstanceRenderProxy& proxy : renderData.meshInstances)
    {
        if (proxy.meshShared)
            m_subInstanceCount += static_cast<uint32_t>(proxy.meshShared->geometries.size());
    }

    tlasDesc.topLevelMaxInstances = std::max<size_t>(1, renderData.meshInstanceEntities.size());
    assert(tlasDesc.topLevelMaxInstances < (1 << 15));
    m_topLevelAS = m_device->createAccelStruct(tlasDesc);

    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(SubInstanceData) * std::max(1u, m_subInstanceCount);
    bufferDesc.debugName = "Instances";
    bufferDesc.structStride = sizeof(SubInstanceData);
    bufferDesc.canHaveRawViews = false;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.isVertexBuffer = false;
    bufferDesc.initialState = nvrhi::ResourceStates::Common;
    bufferDesc.keepInitialState = true;
    m_subInstanceBuffer = m_device->createBuffer(bufferDesc);

    m_subInstanceData.clear();
    m_subInstanceData.assign(m_subInstanceCount, SubInstanceData{});
}

void AccelStructManager::uploadSubInstanceData(nvrhi::ICommandList* commandList) const
{
    assert(m_subInstanceCount == m_subInstanceData.size());
    if (m_subInstanceData.empty())
        return;
    commandList->writeBuffer(m_subInstanceBuffer, m_subInstanceData.data(),
                             m_subInstanceData.size() * sizeof(SubInstanceData));
}

void AccelStructManager::clearMeshAccelStructs(Scene& scene)
{
    for (const std::shared_ptr<MeshInfo>& mesh : scene.getMeshes())
    {
        mesh->accelStruct = nullptr;
        mesh->AccelStructOMM = nullptr;
        mesh->OpacityMicroMaps.clear();
        mesh->DebugData = nullptr;
        mesh->DebugDataDirty = true;
    }
    m_meshesPendingAccelRebuild.clear();
}

void AccelStructManager::requestMeshRebuild(const std::shared_ptr<MeshInfo>& mesh)
{
    if (!mesh)
        return;

    if (!m_topLevelAS)
        return;

    const auto it = std::find(m_meshesPendingAccelRebuild.begin(), m_meshesPendingAccelRebuild.end(), mesh);
    if (it == m_meshesPendingAccelRebuild.end())
        m_meshesPendingAccelRebuild.push_back(mesh);
}

void AccelStructManager::rebuildDirtyMeshes(nvrhi::ICommandList*            commandList,
                                            const Scene&                    scene,
                                            const AccelStructBuildSettings& settings,
                                            bool&                           fullRebuildRequested)
{
    if (m_meshesPendingAccelRebuild.empty())
        return;

    std::vector<std::shared_ptr<MeshInfo>> dirtyMeshes;
    dirtyMeshes.swap(m_meshesPendingAccelRebuild);

    for (const std::shared_ptr<MeshInfo>& mesh : dirtyMeshes)
    {
        if (!mesh || mesh->isSkinPrototype)
            continue;

        if (!mesh->buffers || !mesh->buffers->vertexBuffer || !mesh->buffers->indexBuffer)
        {
            fullRebuildRequested = true;
            continue;
        }

        bvh::Config cfg = { .excludeTransmissive = settings.excludeTransmissive };
        // Geometry-sequence / point-cache meshes keep fixed topology; rebuild like skinned
        // meshes via AllowUpdate + PerformUpdate so we do not allocate a new BLAS every frame.
        nvrhi::rt::AccelStructDesc blasDesc = bvh::getMeshBlasDesc(cfg, *mesh, nullptr, true);
        assert((int)blasDesc.bottomLevelGeometries.size() < (1 << 12));
        if (blasDesc.bottomLevelGeometries.empty())
            continue;

        const bool canUpdateInPlace = mesh->accelStruct
            && (mesh->accelStruct->getDesc().buildFlags & nvrhi::rt::AccelStructBuildFlags::AllowUpdate) != 0;

        if (canUpdateInPlace)
        {
            blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastBuild
                | nvrhi::rt::AccelStructBuildFlags::PerformUpdate;
            nvrhi::utils::BuildBottomLevelAccelStruct(commandList, mesh->accelStruct, blasDesc);
        }
        else
        {
            blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastBuild
                | nvrhi::rt::AccelStructBuildFlags::AllowUpdate;
            nvrhi::rt::AccelStructHandle as = m_device->createAccelStruct(blasDesc);
            nvrhi::utils::BuildBottomLevelAccelStruct(commandList, as, blasDesc);
            mesh->accelStruct = as;

            // Recreating the BLAS invalidates any OMM attachment on the previous AS.
            mesh->AccelStructOMM = nullptr;
            mesh->OpacityMicroMaps.clear();
            mesh->DebugData = nullptr;
            mesh->DebugDataDirty = true;
        }
    }
}

void AccelStructManager::updateSkinnedBlases(nvrhi::ICommandList*            commandList,
                                             const Scene&                    scene,
                                             const AccelStructBuildSettings& settings,
                                             uint32_t                        /*frameIndex*/) const
{
    commandList->beginMarker("Skinned BLAS Updates");
    uint32_t skinnedUpdateCount = 0;
    uint32_t skippedEmptySkinnedUpdateCount = 0;
    uint32_t skippedDuplicateSkinnedUpdateCount = 0;
    std::unordered_set<const MeshInfo*> preparedSkinnedMeshes;
    std::unordered_set<const MeshInfo*> updatedSkinnedMeshes;

    const scene::SceneRenderData& renderData = scene.getRenderData();

    for (const scene::SkinnedMeshRenderProxy& proxy : renderData.skinnedMeshes)
    {
        if (!proxy.needsSkinningUpdate || !proxy.mesh)
            continue;

        if (!proxy.mesh->accelStruct
            || !proxy.mesh->buffers
            || !proxy.mesh->buffers->vertexBuffer)
        {
            continue;
        }
        if (!preparedSkinnedMeshes.insert(proxy.mesh.get()).second)
            continue;

        commandList->setAccelStructState(proxy.mesh->accelStruct, nvrhi::ResourceStates::AccelStructWrite);
        commandList->setBufferState(proxy.mesh->buffers->vertexBuffer,
                                    nvrhi::ResourceStates::AccelStructBuildInput);
    }
    commandList->commitBarriers();

    for (const scene::SkinnedMeshRenderProxy& proxy : renderData.skinnedMeshes)
    {
        if (!proxy.needsSkinningUpdate || !proxy.mesh || !proxy.mesh->accelStruct)
            continue;

        if (!updatedSkinnedMeshes.insert(proxy.mesh.get()).second)
        {
            skippedDuplicateSkinnedUpdateCount++;
            continue;
        }

        bvh::Config cfg = { .excludeTransmissive = settings.excludeTransmissive };
        nvrhi::rt::AccelStructDesc blasDesc = bvh::getMeshBlasDesc(cfg, *proxy.mesh, nullptr, true);
        if (blasDesc.bottomLevelGeometries.empty())
        {
            skippedEmptySkinnedUpdateCount++;
            continue;
        }
        nvrhi::utils::BuildBottomLevelAccelStruct(commandList, proxy.mesh->accelStruct, blasDesc);
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

void AccelStructManager::buildTlas(nvrhi::ICommandList*            commandList,
                                   const Scene&                    scene,
                                   const AccelStructBuildSettings& settings,
                                   const OmmAccelStructState&      ommState,
                                   OpacityMicromapBuilder*                       opacityMicromapBuilder) const
{
    std::vector<nvrhi::rt::InstanceDesc> instances;

    uint subInstanceCount = 0;
    const scene::SceneRenderData& renderData = scene.getRenderData();
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
        if (proxy.meshShared
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

        nvrhi::rt::InstanceDesc instanceDesc;
        instanceDesc.instanceID = compactedGeometryInstanceIndex;
        instanceDesc.instanceContributionToHitGroupIndex = compactedGeometryInstanceIndex;
        instanceDesc.flags = nvrhi::rt::InstanceFlags::None;
        dm::affineToColumnMajor(proxy.transformFloat, instanceDesc.transform);

        if (!proxy.meshShared)
        {
            instanceDesc.bottomLevelAS = nullptr;
            instanceDesc.instanceMask = 0;
            instances.push_back(instanceDesc);
            continue;
        }

        const std::shared_ptr<MeshInfo>& mesh = proxy.meshShared;
        const uint32_t meshSubInstanceCount = (uint32_t)mesh->geometries.size();

        const bool hasAttachementOMM = opacityMicromapBuilder && mesh->AccelStructOMM.Get() != nullptr;
        const bool useOmmBLAS = ommState.enabled && hasAttachementOMM && !settings.forceOpaque && !ommState.debugViewEnabled;
        auto* bottomLevelAS = useOmmBLAS ? mesh->AccelStructOMM.Get() : mesh->accelStruct.Get();

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
        instanceDesc.flags = ommState.force2State ? nvrhi::rt::InstanceFlags::ForceOMM2State : nvrhi::rt::InstanceFlags::None;
        if (settings.forceOpaque || ommState.debugViewEnabled)
            instanceDesc.flags = (nvrhi::rt::InstanceFlags)((uint32_t)instanceDesc.flags | (uint32_t)nvrhi::rt::InstanceFlags::ForceOpaque);

        subInstanceCount += meshSubInstanceCount;
        instances.push_back(instanceDesc);
    }
    assert(m_subInstanceCount == subInstanceCount);
    assert(instances.size() == renderData.meshInstances.size());

    commandList->beginMarker("TLAS update");
    commandList->buildTopLevelAccelStruct(
        m_topLevelAS,
        instances.empty() ? nullptr : instances.data(),
        instances.size(),
        nvrhi::rt::AccelStructBuildFlags::AllowEmptyInstances);
    commandList->endMarker();
}

void AccelStructManager::releaseGpuResources()
{
    m_topLevelAS = nullptr;
    m_subInstanceBuffer = nullptr;
    m_subInstanceData.clear();
    m_subInstanceCount = 0;
    m_meshesPendingAccelRebuild.clear();
}

} // namespace caustica

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

    for (const std::shared_ptr<MeshInfo>& mesh : scene.GetMeshes())
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
    const scene::SceneRenderData& renderData = scene.GetRenderData();
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
    for (const std::shared_ptr<MeshInfo>& mesh : scene.GetMeshes())
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
                                             uint32_t                        frameIndex) const
{
    commandList->beginMarker("Skinned BLAS Updates");
    uint32_t skinnedUpdateCount = 0;
    uint32_t skippedEmptySkinnedUpdateCount = 0;
    uint32_t skippedDuplicateSkinnedUpdateCount = 0;
    std::unordered_set<const MeshInfo*> preparedSkinnedMeshes;
    std::unordered_set<const MeshInfo*> updatedSkinnedMeshes;

    const scene::SceneRenderData& renderData = scene.GetRenderData();

    for (const scene::SkinnedMeshRenderProxy& proxy : renderData.skinnedMeshes)
    {
        if (!proxy.skinned || !proxy.meshInstance)
            continue;

        scene::SkinnedMeshComponent& skinned = *proxy.skinned;
        scene::MeshInstanceComponent& meshComp = *proxy.meshInstance;

        if (skinned.lastUpdateFrameIndex != scene::kForceSkinnedMeshUpdateFrameIndex
            && skinned.lastUpdateFrameIndex < frameIndex
            || !meshComp.mesh
            || !meshComp.mesh->accelStruct
            || !meshComp.mesh->buffers
            || !meshComp.mesh->buffers->vertexBuffer)
        {
            continue;
        }
        if (!preparedSkinnedMeshes.insert(meshComp.mesh.get()).second)
            continue;

        commandList->setAccelStructState(meshComp.mesh->accelStruct, nvrhi::ResourceStates::AccelStructWrite);
        commandList->setBufferState(meshComp.mesh->buffers->vertexBuffer,
                                    nvrhi::ResourceStates::AccelStructBuildInput);
    }
    commandList->commitBarriers();

    for (const scene::SkinnedMeshRenderProxy& proxy : renderData.skinnedMeshes)
    {
        if (!proxy.skinned || !proxy.meshInstance)
            continue;

        scene::SkinnedMeshComponent& skinned = *proxy.skinned;
        scene::MeshInstanceComponent& meshComp = *proxy.meshInstance;

        if (skinned.lastUpdateFrameIndex != scene::kForceSkinnedMeshUpdateFrameIndex
            && skinned.lastUpdateFrameIndex < frameIndex
            || !meshComp.mesh
            || !meshComp.mesh->accelStruct)
        {
            continue;
        }
        if (!updatedSkinnedMeshes.insert(meshComp.mesh.get()).second)
        {
            skippedDuplicateSkinnedUpdateCount++;
            continue;
        }

        bvh::Config cfg = { .excludeTransmissive = settings.excludeTransmissive };
        nvrhi::rt::AccelStructDesc blasDesc = bvh::getMeshBlasDesc(cfg, *meshComp.mesh, nullptr, true);
        if (blasDesc.bottomLevelGeometries.empty())
        {
            skippedEmptySkinnedUpdateCount++;
            continue;
        }
        nvrhi::utils::BuildBottomLevelAccelStruct(commandList, meshComp.mesh->accelStruct, blasDesc);
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
    const scene::SceneRenderData& renderData = scene.GetRenderData();
    for (const scene::MeshInstanceRenderProxy& proxy : renderData.meshInstances)
    {
        if (!proxy.meshShared)
            continue;

        const std::shared_ptr<MeshInfo>& mesh = proxy.meshShared;

        const bool hasAttachementOMM = opacityMicromapBuilder && mesh->AccelStructOMM.Get() != nullptr;
        const bool useOmmBLAS = ommState.enabled && hasAttachementOMM && !settings.forceOpaque && !ommState.debugViewEnabled;

        const uint32_t meshSubInstanceCount = (uint32_t)mesh->geometries.size();
        // geometryInstanceIndex must be a dense prefix sum after refreshInstanceIndices.
        // Prefer the compacted running count if a stale snapshot slips through.
        if (subInstanceCount != static_cast<uint32_t>(proxy.geometryInstanceIndex))
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

        auto* bottomLevelAS = useOmmBLAS ? mesh->AccelStructOMM.Get() : mesh->accelStruct.Get();
        if (bottomLevelAS == nullptr)
        {
            static bool warnedNullBlas = false;
            if (!warnedNullBlas)
            {
                warning("BuildTLAS skipped one or more mesh instances with null BLAS to avoid invalid TLAS input.");
                warnedNullBlas = true;
            }
            subInstanceCount += meshSubInstanceCount;
            continue;
        }

        nvrhi::rt::InstanceDesc instanceDesc;
        instanceDesc.bottomLevelAS = bottomLevelAS;
        instanceDesc.instanceMask = (ommState.onlyOMMs && !hasAttachementOMM) ? 0 : 1;
        instanceDesc.instanceID = compactedGeometryInstanceIndex;
        instanceDesc.instanceContributionToHitGroupIndex = compactedGeometryInstanceIndex;
        instanceDesc.flags = ommState.force2State ? nvrhi::rt::InstanceFlags::ForceOMM2State : nvrhi::rt::InstanceFlags::None;
        if (settings.forceOpaque || ommState.debugViewEnabled)
            instanceDesc.flags = (nvrhi::rt::InstanceFlags)((uint32_t)instanceDesc.flags | (uint32_t)nvrhi::rt::InstanceFlags::ForceOpaque);

        subInstanceCount += meshSubInstanceCount;

        dm::affineToColumnMajor(proxy.transformFloat, instanceDesc.transform);

        instances.push_back(instanceDesc);
    }
    assert(m_subInstanceCount == subInstanceCount);

    commandList->beginMarker("TLAS Update");
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

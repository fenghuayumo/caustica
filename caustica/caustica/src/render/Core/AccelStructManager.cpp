#include <render/Core/AccelStructManager.h>

#include <render/Core/AccelerationStructureUtil.h>
#include <render/Passes/OMM/OpacityMicromapBuilder.h>
#include <scene/Scene.h>
#include <scene/SceneGraph.h>
#include <core/log.h>
#include <rhi/utils.h>

#include <algorithm>

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
    for (const std::shared_ptr<MeshInfo>& mesh : scene.GetSceneGraph()->GetMeshes())
    {
        if (mesh->isSkinPrototype)
            continue;

        bvh::Config cfg = { .excludeTransmissive = settings.excludeTransmissive };

        nvrhi::rt::AccelStructDesc blasDesc = bvh::GetMeshBlasDesc(cfg, *mesh, nullptr, false);
        assert((int)blasDesc.bottomLevelGeometries.size() < (1 << 12));

        nvrhi::rt::AccelStructHandle as = m_device->createAccelStruct(blasDesc);
        nvrhi::utils::BuildBottomLevelAccelStruct(commandList, as, blasDesc);
        mesh->accelStruct = as;
    }
}

void AccelStructManager::createTlas(nvrhi::ICommandList* commandList, const Scene& scene)
{
    (void)commandList;

    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.isTopLevel = true;
    tlasDesc.topLevelMaxInstances = std::max<size_t>(1, scene.GetSceneGraph()->GetMeshInstances().size());
    tlasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
    assert(tlasDesc.topLevelMaxInstances < (1 << 15));
    m_topLevelAS = m_device->createAccelStruct(tlasDesc);

    m_subInstanceCount = 0;
    for (const auto& instance : scene.GetSceneGraph()->GetMeshInstances())
        m_subInstanceCount += (uint32_t)instance->GetMesh()->geometries.size();

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
    for (const std::shared_ptr<MeshInfo>& mesh : scene.GetSceneGraph()->GetMeshes())
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
        nvrhi::rt::AccelStructDesc blasDesc = bvh::GetMeshBlasDesc(cfg, *mesh, nullptr, false);
        blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastBuild;
        assert((int)blasDesc.bottomLevelGeometries.size() < (1 << 12));

        nvrhi::rt::AccelStructHandle as = m_device->createAccelStruct(blasDesc);
        nvrhi::utils::BuildBottomLevelAccelStruct(commandList, as, blasDesc);
        mesh->accelStruct = as;

        mesh->AccelStructOMM = nullptr;
        mesh->OpacityMicroMaps.clear();
        mesh->DebugData = nullptr;
        mesh->DebugDataDirty = true;
    }
}

void AccelStructManager::updateSkinnedBlases(nvrhi::ICommandList*            commandList,
                                             const Scene&                    scene,
                                             const AccelStructBuildSettings& settings,
                                             uint32_t                        frameIndex) const
{
    commandList->beginMarker("Skinned BLAS Updates");

    for (const auto& skinnedInstance : scene.GetSceneGraph()->GetSkinnedMeshInstances())
    {
        if (skinnedInstance->GetLastUpdateFrameIndex() < frameIndex)
            continue;

        commandList->setAccelStructState(skinnedInstance->GetMesh()->accelStruct, nvrhi::ResourceStates::AccelStructWrite);
        commandList->setBufferState(skinnedInstance->GetMesh()->buffers->vertexBuffer,
                                    nvrhi::ResourceStates::AccelStructBuildInput);
    }
    commandList->commitBarriers();

    for (const auto& skinnedInstance : scene.GetSceneGraph()->GetSkinnedMeshInstances())
    {
        if (skinnedInstance->GetLastUpdateFrameIndex() < frameIndex)
            continue;

        bvh::Config cfg = { .excludeTransmissive = settings.excludeTransmissive };
        nvrhi::rt::AccelStructDesc blasDesc = bvh::GetMeshBlasDesc(cfg, *skinnedInstance->GetMesh(), nullptr, true);
        nvrhi::utils::BuildBottomLevelAccelStruct(commandList, skinnedInstance->GetMesh()->accelStruct, blasDesc);
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
    for (const auto& instance : scene.GetSceneGraph()->GetMeshInstances())
    {
        const std::shared_ptr<MeshInfo>& mesh = instance->GetMesh();

        const bool hasAttachementOMM = opacityMicromapBuilder && mesh->AccelStructOMM.Get() != nullptr;
        const bool useOmmBLAS = ommState.enabled && hasAttachementOMM && !settings.forceOpaque && !ommState.debugViewEnabled;

        const uint32_t meshSubInstanceCount = (uint32_t)mesh->geometries.size();
        assert(subInstanceCount == instance->GetGeometryInstanceIndex());

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
        instanceDesc.instanceID = instance->GetGeometryInstanceIndex();
        instanceDesc.instanceContributionToHitGroupIndex = subInstanceCount;
        instanceDesc.flags = ommState.force2State ? nvrhi::rt::InstanceFlags::ForceOMM2State : nvrhi::rt::InstanceFlags::None;
        if (settings.forceOpaque || ommState.debugViewEnabled)
            instanceDesc.flags = (nvrhi::rt::InstanceFlags)((uint32_t)instanceDesc.flags | (uint32_t)nvrhi::rt::InstanceFlags::ForceOpaque);

        subInstanceCount += meshSubInstanceCount;

        auto node = instance->GetNode();
        assert(node);
        dm::affineToColumnMajor(node->GetLocalToWorldTransformFloat(), instanceDesc.transform);

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

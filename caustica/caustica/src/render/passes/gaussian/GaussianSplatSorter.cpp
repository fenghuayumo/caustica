#include <render/passes/gaussian/GaussianSplatSorter.h>

#include <render/passes/gaussian/GaussianSplatPass.h>
#include <render/gpuSort/GPUSort.h>

#include <algorithm>
#include <numeric>
#include <random>

using namespace caustica::math;

namespace caustica::render
{

namespace
{
    bool matrixEquals(const float4x4& a, const float4x4& b)
    {
        for (int index = 0; index < 16; ++index)
        {
            if (a.m_data[index] != b.m_data[index])
                return false;
        }
        return true;
    }
}

void GaussianSplatSorter::invalidate()
{
    m_sortCacheValid = false;
    m_cachedSortSplatCount = 0;
}

void GaussianSplatSorter::onSplatCountChanged(uint32_t splatCount)
{
    m_randomIndices.clear();
    m_randomIndexUploadPending = true;
    m_cachedSortSplatCount = splatCount;
    invalidate();
}

void GaussianSplatSorter::uploadStochasticSplatIndices(
    caustica::rhi::ICommandList* commandList,
    const GaussianSplatSortResources& resources)
{
    if (!resources.indexBuffer || resources.splatCount == 0)
        return;

    if (m_randomIndices.size() != resources.splatCount)
    {
        m_randomIndices.resize(resources.splatCount);
        std::iota(m_randomIndices.begin(), m_randomIndices.end(), 0u);
        std::mt19937 rng(0x3d05da7au);
        std::shuffle(m_randomIndices.begin(), m_randomIndices.end(), rng);
        m_randomIndexUploadPending = true;
    }

    if (!m_randomIndexUploadPending)
        return;

    commandList->writeBuffer(resources.indexBuffer, m_randomIndices.data(), m_randomIndices.size() * sizeof(uint32_t));
    m_randomIndexUploadPending = false;
}

void GaussianSplatSorter::buildDistanceCulledSplatList(
    caustica::rhi::ICommandList* commandList,
    GaussianSplatSortMode sortMode,
    const GaussianSplatSortResources& resources)
{
    if (!resources.sortKeyBindingSet || !resources.sortKeyPipeline || !resources.sortControlBuffer || !resources.drawIndirectBuffer)
        return;

    const uint32_t zero = 0;
    caustica::rhi::DrawIndirectArguments drawArgs = {};
    drawArgs.instanceCount = 1;
    commandList->writeBuffer(resources.sortControlBuffer, &zero, sizeof(zero));
    commandList->writeBuffer(resources.drawIndirectBuffer, &drawArgs, sizeof(drawArgs));

    {
        caustica::rhi::ComputeState state;
        state.pipeline = resources.sortKeyPipeline;
        state.bindings = { resources.sortKeyBindingSet };

        commandList->setBufferState(resources.sortKeyBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(resources.indexBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(resources.sortControlBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(resources.drawIndirectBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();

        commandList->setComputeState(state);
        commandList->dispatch((resources.splatCount + 255u) / 256u, 1, 1);
    }

    if (sortMode == GaussianSplatSortMode::GpuSort && resources.gpuSort)
    {
        commandList->setBufferState(resources.sortKeyBuffer, caustica::rhi::ResourceStates::ShaderResource);
        commandList->setBufferState(resources.indexBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(resources.sortControlBuffer, caustica::rhi::ResourceStates::CopySource);
        commandList->commitBarriers();

        resources.gpuSort->sort(
            commandList,
            resources.sortControlBuffer,
            0,
            resources.sortKeyBuffer,
            resources.indexBuffer,
            resources.splatCount,
            false);
    }

    m_cachedSortMode = sortMode;
    m_sortCacheValid = false;
    m_randomIndexUploadPending = true;
}

bool GaussianSplatSorter::canReuseSort(
    const GaussianSplatConstants& constants,
    const GaussianSplatSortResources& resources) const
{
    return m_sortCacheValid
        && m_cachedSortSplatCount == resources.splatCount
        && matrixEquals(m_cachedSortWorldToClipNoOffset, constants.view.matWorldToClipNoOffset)
        && matrixEquals(m_cachedSortObjectToWorld, constants.objectToWorld);
}

void GaussianSplatSorter::updateIndices(
    caustica::rhi::ICommandList* commandList,
    const GaussianSplatConstants& constants,
    GaussianSplatSortMode sortMode,
    const GaussianSplatSortResources& resources)
{
    if (constants.frustumCulling == uint32_t(GaussianSplatFrustumCulling::AtDistanceStage))
    {
        buildDistanceCulledSplatList(commandList, sortMode, resources);
        return;
    }

    if (sortMode != m_cachedSortMode && sortMode == GaussianSplatSortMode::StochasticSplats)
        m_randomIndexUploadPending = true;

    if (sortMode == GaussianSplatSortMode::StochasticSplats)
    {
        uploadStochasticSplatIndices(commandList, resources);
        m_cachedSortMode = sortMode;
        m_sortCacheValid = false;
        return;
    }

    if (!resources.gpuSort || !resources.sortKeyBindingSet || !resources.sortKeyPipeline
        || !resources.sortControlBuffer || !resources.drawIndirectBuffer)
    {
        return;
    }

    if (m_cachedSortMode == sortMode && canReuseSort(constants, resources))
        return;

    {
        caustica::rhi::ComputeState state;
        state.pipeline = resources.sortKeyPipeline;
        state.bindings = { resources.sortKeyBindingSet };

        commandList->setBufferState(resources.sortKeyBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(resources.indexBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(resources.sortControlBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(resources.drawIndirectBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();

        commandList->setComputeState(state);
        commandList->dispatch((resources.splatCount + 255u) / 256u, 1, 1);
    }

    commandList->writeBuffer(resources.sortControlBuffer, &resources.splatCount, sizeof(resources.splatCount));

    commandList->setBufferState(resources.sortKeyBuffer, caustica::rhi::ResourceStates::ShaderResource);
    commandList->setBufferState(resources.indexBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(resources.sortControlBuffer, caustica::rhi::ResourceStates::CopySource);
    commandList->commitBarriers();

    resources.gpuSort->sort(
        commandList,
        resources.sortControlBuffer,
        0,
        resources.sortKeyBuffer,
        resources.indexBuffer,
        resources.splatCount,
        true);

    m_cachedSortWorldToClipNoOffset = constants.view.matWorldToClipNoOffset;
    m_cachedSortObjectToWorld = constants.objectToWorld;
    m_cachedSortSplatCount = resources.splatCount;
    m_cachedSortMode = sortMode;
    m_sortCacheValid = true;
}

} // namespace caustica::render

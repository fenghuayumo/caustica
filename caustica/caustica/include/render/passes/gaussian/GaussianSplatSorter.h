#pragma once

#include <math/math.h>
#include <rhi/rhi.h>

#include <cstdint>
#include <memory>
#include <vector>

#include <shaders/SampleConstantBuffer.h>

class GPUSort;

enum class GaussianSplatSortMode : uint32_t
{
    GpuSort = 0,
    StochasticSplats = 1
};

namespace caustica::render
{

struct GaussianSplatSortResources
{
    caustica::rhi::IBuffer* sortKeyBuffer = nullptr;
    caustica::rhi::IBuffer* indexBuffer = nullptr;
    caustica::rhi::IBuffer* sortControlBuffer = nullptr;
    caustica::rhi::IBuffer* drawIndirectBuffer = nullptr;
    caustica::rhi::BindingSetHandle sortKeyBindingSet;
    caustica::rhi::ComputePipelineHandle sortKeyPipeline;
    std::shared_ptr<GPUSort> gpuSort;
    uint32_t splatCount = 0;
};

class GaussianSplatSorter
{
public:
    void invalidate();
    void onSplatCountChanged(uint32_t splatCount);

    void updateIndices(
        caustica::rhi::ICommandList* commandList,
        const GaussianSplatConstants& constants,
        GaussianSplatSortMode sortMode,
        const GaussianSplatSortResources& resources);

private:
    void buildDistanceCulledSplatList(
        caustica::rhi::ICommandList* commandList,
        GaussianSplatSortMode sortMode,
        const GaussianSplatSortResources& resources);

    void uploadStochasticSplatIndices(
        caustica::rhi::ICommandList* commandList,
        const GaussianSplatSortResources& resources);

    [[nodiscard]] bool canReuseSort(
        const GaussianSplatConstants& constants,
        const GaussianSplatSortResources& resources) const;

    bool m_sortCacheValid = false;
    uint32_t m_cachedSortSplatCount = 0;
    GaussianSplatSortMode m_cachedSortMode = GaussianSplatSortMode::GpuSort;
    caustica::math::float4x4 m_cachedSortWorldToClipNoOffset = caustica::math::float4x4::identity();
    caustica::math::float4x4 m_cachedSortObjectToWorld = caustica::math::float4x4::identity();
    std::vector<uint32_t> m_randomIndices;
    bool m_randomIndexUploadPending = true;
};

} // namespace caustica::render

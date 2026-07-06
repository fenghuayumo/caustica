#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>

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
    nvrhi::IBuffer* sortKeyBuffer = nullptr;
    nvrhi::IBuffer* indexBuffer = nullptr;
    nvrhi::IBuffer* sortControlBuffer = nullptr;
    nvrhi::IBuffer* drawIndirectBuffer = nullptr;
    nvrhi::BindingSetHandle sortKeyBindingSet;
    nvrhi::ComputePipelineHandle sortKeyPipeline;
    std::shared_ptr<GPUSort> gpuSort;
    uint32_t splatCount = 0;
};

class GaussianSplatSorter
{
public:
    void invalidate();
    void onSplatCountChanged(uint32_t splatCount);

    void updateIndices(
        nvrhi::ICommandList* commandList,
        const GaussianSplatConstants& constants,
        GaussianSplatSortMode sortMode,
        const GaussianSplatSortResources& resources);

private:
    void buildDistanceCulledSplatList(
        nvrhi::ICommandList* commandList,
        GaussianSplatSortMode sortMode,
        const GaussianSplatSortResources& resources);

    void uploadStochasticSplatIndices(
        nvrhi::ICommandList* commandList,
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

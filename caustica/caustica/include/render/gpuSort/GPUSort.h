#pragma once

#include <render/core/BindingCache.h>
#include <rhi/rhi.h>
#include <math/math.h>
#include <memory>

#include <math/math.h>

#include <filesystem>

using namespace caustica::math;

namespace caustica
{
    class ShaderFactory;
}

class ShaderDebug;

// This uses older version of AMD's https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/docs/techniques/parallel-sort.md
// that was ported from https://github.com/GameTechDev/XeGTAO/blob/master/Source/Rendering/engine/shaders/FFX_ParallelSort.h 
// See sort() below for HowTo's.

class GPUSort
{
public:
    GPUSort(caustica::rhi::Device* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory);
    ~GPUSort();

    void                            createRenderPasses(std::shared_ptr<ShaderDebug> shaderDebug);

    // Provide number of items to be sorted as a uint32 variable within the controlBuffer at the itemCountByteOffset. Must be less than or equal to maxItemCount. If bigger, behaviour is undefined.
    // Provide sort keys in the keys buffer (uint32). These will NOT be touched - only indices are sorted.
    // Provide indices in the indices buffer (uint32). These have to be already initialized to [1, 2, 3, 4, 5, ..., itemCount-1] and will be sorted in-place. Also known as the "payload". If you do not want to pre-initialize them, use `resetIndices`
    // Provide maxItemCount as the maximum possible itemCount - this will be used to create needed scratch buffers at runtime.
    // Do all necessary resource barriers before and after the call to sort.
    void                            sort(caustica::rhi::CommandList * commandList, caustica::rhi::BufferHandle controlBuffer, uint itemCountByteOffset, caustica::rhi::BufferHandle bufferKeys, caustica::rhi::BufferHandle bufferIndices, uint maxItemCount, bool resetIndices);

private:
    void                            reCreateWorkingBuffers(uint32_t maxItemCount);

private:
    caustica::rhi::DeviceHandle             m_device;
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
    std::shared_ptr<ShaderDebug>   m_shaderDebug;

    caustica::rhi::ShaderHandle             m_CSSetupIndirect;
    caustica::rhi::ComputePipelineHandle    m_PSOSetupIndirect;
    caustica::rhi::ShaderHandle             m_CSCount;
    caustica::rhi::ComputePipelineHandle    m_PSOCount;
    caustica::rhi::ShaderHandle             m_CSCountIIFP;      // init indices first pass
    caustica::rhi::ComputePipelineHandle    m_PSOCountIIFP;     // init indices first pass
    caustica::rhi::ShaderHandle             m_CSCountReduce;
    caustica::rhi::ComputePipelineHandle    m_PSOCountReduce;
    caustica::rhi::ShaderHandle             m_CSScanPrefix;
    caustica::rhi::ComputePipelineHandle    m_PSOScanPrefix;
    caustica::rhi::ShaderHandle             m_CSScanAdd;
    caustica::rhi::ComputePipelineHandle    m_PSOScanAdd;
    caustica::rhi::ShaderHandle             m_CSScatter;
    caustica::rhi::ComputePipelineHandle    m_PSOScatter;
    caustica::rhi::ShaderHandle             m_CSScatterIIFP;    // init indices first pass
    caustica::rhi::ComputePipelineHandle    m_PSOScatterIIFP;   // init indices first pass
    caustica::rhi::ShaderHandle             m_CSValidate;
    caustica::rhi::ComputePipelineHandle    m_PSOValidate;

    caustica::rhi::BindingLayoutHandle      m_initBindingLayout;
    caustica::rhi::BindingLayoutHandle      m_commonBindingLayout;
    caustica::BindingCache     m_bindingCache;

    caustica::rhi::BufferHandle             m_controlBuffer;
    caustica::rhi::BufferHandle             m_dispatchIndirectBuffer;
    caustica::rhi::BufferHandle             m_scratchBuffer;
    caustica::rhi::BufferHandle             m_reducedScratchBuffer;
    caustica::rhi::BufferHandle             m_scratchIndicesBuffer;

    uint32_t                        m_scratchMaxItemCountSize = 0;
};

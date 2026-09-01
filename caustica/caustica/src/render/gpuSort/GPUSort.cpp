#include <render/gpuSort/GPUSort.h>

#include <assets/loader/ShaderFactory.h>
#include <render/core/FramebufferFactory.h>
#include <assets/loader/TextureLoader.h>

#include <rhi/utils.h>

#include <core/file_utils.h>
#include <core/format.h>
#include <core/path_utils.h>
#include <core/progress.h>
#include <core/Timer.h>
#include <core/system_utils.h>
#include <core/command_line.h>
#include <core/scope.h>
#include <core/ThreadContext.h>
#include <render/core/ScopedPerfMarker.h>
#include <render/core/TextureUtils.h>
#include <render/passes/debug/ShaderDebug.h>

#define FFX_CPP
#include <shaders/render/gpuSort/FFX_ParallelSort.h>

using namespace caustica::math;
using namespace caustica;


GPUSort::GPUSort(caustica::rhi::Device* device, std::shared_ptr<caustica::ShaderFactory> shaderFactory)
    : m_device(device)
    , m_bindingCache(device)
    , m_shaderFactory(shaderFactory)
{
}


GPUSort::~GPUSort()
{
}

void GPUSort::createRenderPasses(std::shared_ptr<ShaderDebug> shaderDebug)
{
    m_shaderDebug = shaderDebug;

    {
        caustica::rhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = caustica::rhi::ShaderType::Compute;
        layoutDesc.bindings = {
            //caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0),
            caustica::rhi::BindingLayoutItem::PushConstants(1, 4*sizeof(uint32_t)),
            caustica::rhi::BindingLayoutItem::TypedBuffer_SRV(0),       // SrcKeys
            caustica::rhi::BindingLayoutItem::TypedBuffer_SRV(1),       // SrcIndices
            caustica::rhi::BindingLayoutItem::TypedBuffer_UAV(0),       // ScratchBuffer
            caustica::rhi::BindingLayoutItem::TypedBuffer_UAV(1),       // ReducedScratchBuffer
            caustica::rhi::BindingLayoutItem::TypedBuffer_UAV(2),       // DstIndices
            caustica::rhi::BindingLayoutItem::StructuredBuffer_UAV(3),  // ConstsUAV
            //caustica::rhi::BindingLayoutItem::StructuredBuffer_UAV(4),  // DispIndUAV
            caustica::rhi::BindingLayoutItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX),
        };
        m_commonBindingLayout = m_device->createBindingLayout(layoutDesc);


        layoutDesc.bindings = {
            caustica::rhi::BindingLayoutItem::StructuredBuffer_UAV(3),  // ConstsUAV
            caustica::rhi::BindingLayoutItem::StructuredBuffer_UAV(4),  // DispIndUAV
            //caustica::rhi::BindingLayoutItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX),
        };
        m_initBindingLayout = m_device->createBindingLayout(layoutDesc);
    }

    auto CreateCSPSOPair = [&]( caustica::rhi::ShaderHandle & shaderHandle, caustica::rhi::ComputePipelineHandle & psoHandle, const std::string & name, bool initOnly, bool specialInitIndicesFirstPass )
    {
        std::vector<caustica::ShaderMacro> shaderMacros;

        if( specialInitIndicesFirstPass )
            shaderMacros.push_back(caustica::ShaderMacro({ "CAUSTICA_GPUSORT_FIRST_PASS_INIT_INDICES", "1" }));

        shaderHandle = m_shaderFactory->createShader("caustica/shaders/render/gpuSort/GPUSort.hlsl", name.c_str(), &shaderMacros, caustica::rhi::ShaderType::Compute);
        caustica::rhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.bindingLayouts = { (initOnly)?(m_initBindingLayout):(m_commonBindingLayout) };
        pipelineDesc.CS = shaderHandle;
        psoHandle = m_device->createComputePipeline(pipelineDesc);
    };

#define CREATE_CS_PSO_PAIR( X, y, z ) CreateCSPSOPair( m_CS##X, m_PSO##X, #X, y, z );

    CREATE_CS_PSO_PAIR(SetupIndirect, true, false);
    CREATE_CS_PSO_PAIR(Count, false, false);
    CREATE_CS_PSO_PAIR(CountIIFP, false, true);
    CREATE_CS_PSO_PAIR(CountReduce, false, false);
    CREATE_CS_PSO_PAIR(ScanPrefix, false, false);
    CREATE_CS_PSO_PAIR(ScanAdd, false, false);
    CREATE_CS_PSO_PAIR(Scatter, false, false);
    CREATE_CS_PSO_PAIR(ScatterIIFP, false, true);
    CREATE_CS_PSO_PAIR(Validate, false, false);

    {
        caustica::rhi::BufferDesc bufferDesc;
        bufferDesc.initialState = caustica::rhi::ResourceStates::UnorderedAccess;
        bufferDesc.keepInitialState = true;
        bufferDesc.canHaveUAVs = true;
        
        // TODO: why not unify these two?
        bufferDesc.byteSize = sizeof(FFX_ParallelSortCB) * 1;
        bufferDesc.structStride = sizeof(FFX_ParallelSortCB);
        bufferDesc.debugName = "FFX_ParallelSortCB";
        m_controlBuffer = m_device->createBuffer(bufferDesc);

        bufferDesc.isDrawIndirectArgs = true;
        bufferDesc.byteSize = sizeof(FFX_DispatchIndirectBuffer) * 1;
        bufferDesc.structStride = sizeof(FFX_DispatchIndirectBuffer);
        bufferDesc.debugName = "FFX_DispatchIndirectBuffer";
        m_dispatchIndirectBuffer = m_device->createBuffer(bufferDesc);
    }

}

void GPUSort::reCreateWorkingBuffers(uint32_t maxItemCount)
{
    caustica::assertRenderThread();
// TODO: test; this should cause crashes.
//    static uint debugTest = 0; debugTest++; 
//    if (debugTest % 1234 == 0) 
//        m_scratchMaxItemCountSize = 0;

    if( maxItemCount <= m_scratchMaxItemCountSize )
        return;
    
    m_scratchMaxItemCountSize = maxItemCount;

    // allocate the scratch buffers needed for radix sort
    uint32_t scratchBufferSize;
    uint32_t reducedScratchBufferSize;
    uint32_t scratchIndicesBufferSize = maxItemCount;
    FFX_ParallelSort_CalculateScratchResourceSize(maxItemCount, scratchBufferSize, reducedScratchBufferSize);

    // RHI handles retire through the device garbage collector. Waiting for the
    // entire GPU here caused a frame hitch whenever a larger splat set appeared.
    m_bindingCache.clear();
    m_scratchBuffer = nullptr;
    m_reducedScratchBuffer = nullptr;
    m_scratchIndicesBuffer = nullptr;

    caustica::rhi::BufferDesc bufferDesc;
    bufferDesc.initialState = caustica::rhi::ResourceStates::UnorderedAccess;
    bufferDesc.keepInitialState = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.format = caustica::rhi::Format::R32_UINT;
    bufferDesc.structStride = 0;
    bufferDesc.canHaveTypedViews = true;

    bufferDesc.byteSize = scratchBufferSize;
    bufferDesc.debugName = "SortScratchBuffer";
    m_scratchBuffer = m_device->createBuffer(bufferDesc);

    bufferDesc.byteSize = reducedScratchBufferSize;
    bufferDesc.debugName = "SortReducedScratchBuffer";
    m_reducedScratchBuffer = m_device->createBuffer(bufferDesc);

    bufferDesc.byteSize = sizeof(uint32_t) * maxItemCount;
    bufferDesc.debugName = "SortScratchIndicesBuffer";
    m_scratchIndicesBuffer = m_device->createBuffer(bufferDesc);
}

GPUSort::BindingSets GPUSort::findBindingSets(
    caustica::rhi::BufferHandle controlBuffer,
    caustica::rhi::BufferHandle bufferKeys,
    caustica::rhi::BufferHandle bufferIndices,
    bool create)
{
    caustica::rhi::BindingSetDesc initDesc, pingDesc, pongDesc;
    initDesc.bindings = {
        caustica::rhi::BindingSetItem::StructuredBuffer_UAV(3, m_controlBuffer),
        caustica::rhi::BindingSetItem::StructuredBuffer_UAV(4, m_dispatchIndirectBuffer),
    };
    pingDesc.bindings = {
        caustica::rhi::BindingSetItem::PushConstants(1, 4*sizeof(uint32_t)),
        caustica::rhi::BindingSetItem::TypedBuffer_SRV(0, bufferKeys),
        caustica::rhi::BindingSetItem::TypedBuffer_SRV(1, bufferIndices),
        caustica::rhi::BindingSetItem::TypedBuffer_UAV(0, m_scratchBuffer),
        caustica::rhi::BindingSetItem::TypedBuffer_UAV(1, m_reducedScratchBuffer),
        caustica::rhi::BindingSetItem::TypedBuffer_UAV(2, m_scratchIndicesBuffer),
        caustica::rhi::BindingSetItem::StructuredBuffer_UAV(3, m_controlBuffer),
        caustica::rhi::BindingSetItem::RawBuffer_UAV(
            SHADER_DEBUG_BUFFER_UAV_INDEX, m_shaderDebug->getGPUWriteBuffer()),
    };
    pongDesc.bindings = {
        caustica::rhi::BindingSetItem::PushConstants(1, 4*sizeof(uint32_t)),
        caustica::rhi::BindingSetItem::TypedBuffer_SRV(0, bufferKeys),
        caustica::rhi::BindingSetItem::TypedBuffer_SRV(1, m_scratchIndicesBuffer),
        caustica::rhi::BindingSetItem::TypedBuffer_UAV(0, m_scratchBuffer),
        caustica::rhi::BindingSetItem::TypedBuffer_UAV(1, m_reducedScratchBuffer),
        caustica::rhi::BindingSetItem::TypedBuffer_UAV(2, bufferIndices),
        caustica::rhi::BindingSetItem::StructuredBuffer_UAV(3, m_controlBuffer),
        caustica::rhi::BindingSetItem::RawBuffer_UAV(
            SHADER_DEBUG_BUFFER_UAV_INDEX, m_shaderDebug->getGPUWriteBuffer()),
    };

    const auto lookup = [this, create](
        const caustica::rhi::BindingSetDesc& desc,
        caustica::rhi::BindingLayout* layout) {
        return create
            ? m_bindingCache.getOrCreateBindingSet(desc, layout)
            : m_bindingCache.getCachedBindingSet(desc, layout);
    };
    return {
        lookup(initDesc, m_initBindingLayout),
        lookup(pingDesc, m_commonBindingLayout),
        lookup(pongDesc, m_commonBindingLayout),
    };
}

void GPUSort::prepare(
    uint32_t maxItemCount,
    caustica::rhi::BufferHandle controlBuffer,
    caustica::rhi::BufferHandle bufferKeys,
    caustica::rhi::BufferHandle bufferIndices)
{
    caustica::assertRenderThread();
    reCreateWorkingBuffers(maxItemCount);
    (void)findBindingSets(controlBuffer, bufferKeys, bufferIndices, true);
}

uint32_t FloorLog2(uint32_t n)  { assert(n > 0); return n == 1 ? 0 : 1 + FloorLog2(n >> 1); }
uint32_t CeilLog2(uint32_t n)   { assert(n > 0); return n == 1 ? 0 : FloorLog2(n - 1) + 1; };

void GPUSort::sort(caustica::rhi::CommandList * commandList, caustica::rhi::BufferHandle controlBuffer, uint32_t itemCountByteOffset, caustica::rhi::BufferHandle bufferKeys, caustica::rhi::BufferHandle bufferIndices, uint32_t maxItemCount, bool resetIndices)
{
    RAII_SCOPE( commandList->beginMarker("GPUSort");, commandList->endMarker(); );

    // prepare() must have run on the render thread before this callback was
    // dispatched to a render-graph recording worker.
    assert(maxItemCount <= m_scratchMaxItemCountSize);

    commandList->copyBuffer( m_controlBuffer, 0, controlBuffer, itemCountByteOffset, sizeof(uint32_t) );

    // // implicit copy barrier should deal with this automatically so not needed
    // commandList->setBufferState(m_controlBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
    // commandList->commitBarriers();

    const BindingSets bindingSets = findBindingSets(
        controlBuffer, bufferKeys, bufferIndices, false);
    assert(bindingSets.init && bindingSets.ping && bindingSets.pong);
    if (!bindingSets.init || !bindingSets.ping || !bindingSets.pong)
        return;

    auto RunCSPass = [ & ]( const char * markerName, const caustica::rhi::ComputePipelineHandle & pso, uint dispatchSizeX, uint indirectDispatchArgOffset, bool pong, uint rootConst, bool initOnly = false )
    {
#if 0 // enable for markers for each pass!
        RAII_SCOPE( commandList->beginMarker(markerName);, commandList->endMarker(); );
#endif

        caustica::rhi::ComputeState state;
        state.bindings = { (!pong)?(bindingSets.ping):(bindingSets.pong) };
        if( initOnly )
            state.bindings = {bindingSets.init};
        state.pipeline = pso;

        if (dispatchSizeX == 0)
            state.indirectParams = m_dispatchIndirectBuffer;

        commandList->setComputeState(state);

        uint4 tinyRootConsts = { rootConst, 0, 0, 0 };
        if( !initOnly )
            commandList->setPushConstants(&tinyRootConsts, sizeof(tinyRootConsts));
        if (dispatchSizeX > 0)
            commandList->dispatch(dispatchSizeX, 1, 1);
        else
            commandList->dispatchIndirect(indirectDispatchArgOffset);
    };

    auto ScratchBarriers = [ & ]()
    {
        commandList->setBufferState(m_scratchBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_reducedScratchBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();
    };

    RunCSPass( "SetupIndirect", m_PSOSetupIndirect, 1, 0, false, 0, true );

    // add barrier to make sure no race condition with subsequent passes 
    commandList->setBufferState(m_controlBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    uint32_t maxKeyValue = 0xFFFFFFFF;  // optimization - sorting smaller keys can be faster! not exposed now, but should work just fine!
    uint32_t bitsNeeded = CeilLog2(maxKeyValue);
    bitsNeeded = std::max(1U, bitsNeeded);
    uint32_t bitsRounding = FFX_PARALLELSORT_SORT_BITS_PER_PASS * 2;
    bitsNeeded = ((bitsNeeded + bitsRounding - 1) / bitsRounding) * bitsRounding;

    bool pong = false;
    for (uint32_t Shift = 0; Shift < bitsNeeded; Shift += FFX_PARALLELSORT_SORT_BITS_PER_PASS)
    {
        RunCSPass("Count", (Shift==0 && resetIndices)?m_PSOCountIIFP:m_PSOCount, 0, 0 * 4, pong, Shift);        // SEE CAUSTICA_GPUSORT_FIRST_PASS_INIT_INDICES
        ScratchBarriers();
        RunCSPass("CountReduce", m_PSOCountReduce, 0, 4 * 4, pong, Shift);
        ScratchBarriers();
        RunCSPass("ScanPrefix", m_PSOScanPrefix, 1, 0, pong, Shift);
        ScratchBarriers();
        RunCSPass("ScanAdd", m_PSOScanAdd, 0, 4 * 4, pong, Shift);
        ScratchBarriers();
        RunCSPass("Scatter", (Shift==0 && resetIndices)?m_PSOScatterIIFP:m_PSOScatter, 0, 0 * 4, pong, Shift);  // SEE CAUSTICA_GPUSORT_FIRST_PASS_INIT_INDICES
        // ScratchBarriers(); not needed because ping pong forces UAV<->SRV transition which adds a barrier

        pong = !pong;
    }

#if 0
    // Will show up in Debug Output
    RunCSPass("Validate", m_PSOValidate, 1, 0 * 0, false, 0);
#endif
}

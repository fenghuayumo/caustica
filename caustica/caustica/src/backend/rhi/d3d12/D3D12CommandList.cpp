#include "D3D12Backend.h"
#include <pix.h>
#include <sstream>

#include <rhi/common/misc.h>

namespace caustica::rhi::d3d12
{
    CommandList::CommandList(Device* device, const Context& context, DeviceResources& resources, const CommandListParameters& params)
        : m_context(context)
        , m_resources(resources)
        , m_device(device)
        , m_queue(device->getQueue(params.queueType))
        , m_uploadManager(context, m_queue, params.uploadChunkSize, params.uploadMaxMemory, false)
        , m_dxrScratchManager(context, m_queue, params.scratchChunkSize, params.scratchMaxMemory, true)
        , m_stateTracker(context.messageCallback)
        , m_desc(params)
    {
#if CAUSTICA_RHI_WITH_AFTERMATH
        if (m_device->isAftermathEnabled())
            m_device->getAftermathCrashDumpHelper().registerAftermathMarkerTracker(&m_aftermathTracker);
#endif
    }

    CommandList::~CommandList()
    {
#if CAUSTICA_RHI_WITH_AFTERMATH
        if (m_device->isAftermathEnabled())
            m_device->getAftermathCrashDumpHelper().unRegisterAftermathMarkerTracker(&m_aftermathTracker);
#endif
    }

    Object CommandList::getNativeObject(ObjectType objectType)
    {
        switch (objectType)
        {
        case ObjectTypes::D3D12_GraphicsCommandList:
            if (m_activeCommandList)
                return Object(m_activeCommandList->commandList.Get());
            else
                return nullptr;

        case ObjectTypes::D3D12_CommandAllocator:
            if (m_activeCommandList)
                return Object(m_activeCommandList->allocator.Get());
            else
                return nullptr;

        case ObjectTypes::CAUSTICA_RHI_D3D12_CommandList:
            return this;

        default:
            return nullptr;
        }
    }

    std::shared_ptr<InternalCommandList> CommandList::createInternalCommandList() const
    {
        auto commandList = std::make_shared<InternalCommandList>();

        D3D12_COMMAND_LIST_TYPE d3dCommandListType;
        switch (m_desc.queueType)
        {
        case CommandQueue::Graphics:
            d3dCommandListType = D3D12_COMMAND_LIST_TYPE_DIRECT;
            break;
        case CommandQueue::Compute:
            d3dCommandListType = D3D12_COMMAND_LIST_TYPE_COMPUTE;
            break;
        case CommandQueue::Copy:
            d3dCommandListType = D3D12_COMMAND_LIST_TYPE_COPY;
            break;

        case CommandQueue::Count:
        default:
            utils::InvalidEnum();
            return nullptr;
        }

        HRESULT hr = m_context.device->CreateCommandAllocator(
            d3dCommandListType,
            IID_PPV_ARGS(&commandList->allocator));
        if (FAILED(hr))
        {
            std::ostringstream ss;
            ss << "CreateCommandAllocator failed, HRESULT=0x" << std::hex << hr
                << ", deviceRemovedReason=0x" << m_context.device->GetDeviceRemovedReason();
            m_context.messageCallback->message(MessageSeverity::Fatal, ss.str().c_str());
            return nullptr;
        }

        hr = m_context.device->CreateCommandList(
            0,
            d3dCommandListType,
            commandList->allocator,
            nullptr,
            IID_PPV_ARGS(&commandList->commandList));
        if (FAILED(hr) || !commandList->commandList)
        {
            std::ostringstream ss;
            ss << "CreateCommandList failed, HRESULT=0x" << std::hex << hr
                << ", deviceRemovedReason=0x" << m_context.device->GetDeviceRemovedReason();
            m_context.messageCallback->message(MessageSeverity::Fatal, ss.str().c_str());
            return nullptr;
        }

        commandList->commandList->QueryInterface(IID_PPV_ARGS(&commandList->commandList4));
        commandList->commandList->QueryInterface(IID_PPV_ARGS(&commandList->commandList6));
#if CAUSTICA_RHI_D3D12_WITH_COOPVEC
        commandList->commandList->QueryInterface(IID_PPV_ARGS(&commandList->commandListPreview));
#endif

#if CAUSTICA_RHI_WITH_AFTERMATH
        if (m_device->isAftermathEnabled())
            GFSDK_Aftermath_DX12_CreateContextHandle(commandList->commandList, &commandList->aftermathContext);
#endif

        return commandList;
    }

    bool CommandList::commitDescriptorHeaps()
    {
        ID3D12DescriptorHeap* heapSRVetc = m_resources.shaderResourceViewHeap.getShaderVisibleHeap();
        ID3D12DescriptorHeap* heapSamplers = m_resources.samplerHeap.getShaderVisibleHeap();

        if (heapSRVetc != m_currentHeapSRVetc || heapSamplers != m_currentHeapSamplers)
        {
            ID3D12DescriptorHeap* heaps[2] = { heapSRVetc, heapSamplers };
            m_activeCommandList->commandList->SetDescriptorHeaps(2, heaps);

            m_currentHeapSRVetc = heapSRVetc;
            m_currentHeapSamplers = heapSamplers;

            m_instance->referencedNativeResources.push_back(heapSRVetc);
            m_instance->referencedNativeResources.push_back(heapSamplers);

            return true;
        }

        return false;
    }

    bool CommandList::allocateUploadBuffer(size_t size, void** pCpuAddress, D3D12_GPU_VIRTUAL_ADDRESS* pGpuAddress)
    {
        return m_uploadManager.suballocateBuffer(size, nullptr, nullptr, nullptr, pCpuAddress, pGpuAddress,
            m_recordingVersion, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    }

    bool CommandList::allocateDxrScratchBuffer(size_t size, void** pCpuAddress, D3D12_GPU_VIRTUAL_ADDRESS* pGpuAddress)
    {
        return m_dxrScratchManager.suballocateBuffer(size, m_activeCommandList->commandList, nullptr, nullptr, pCpuAddress, pGpuAddress,
            m_recordingVersion, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
    }

    D3D12_GPU_VIRTUAL_ADDRESS CommandList::getBufferGpuVA(rhi::Buffer* _buffer)
    {
        if (!_buffer)
            return 0;

        Buffer* buffer = checked_cast<Buffer*>(_buffer);

        if (buffer->desc.isVolatile)
        {
            return m_volatileConstantBufferAddresses[buffer];
        }

        return buffer->gpuVA;
    }

    caustica::rhi::Device* CommandList::getDevice()
    {
        return m_device;
    }

    void CommandList::beginMarker(const char* name)
    {
        PIXBeginEvent(m_activeCommandList->commandList, 0, name);
#if CAUSTICA_RHI_WITH_AFTERMATH
        if (m_device->isAftermathEnabled())
        {
            const size_t aftermathMarker = m_aftermathTracker.pushEvent(name);
            GFSDK_Aftermath_SetEventMarker(m_activeCommandList->aftermathContext, (const void*)aftermathMarker, 0);
        }
#endif
    }

    void CommandList::endMarker()
    {
        PIXEndEvent(m_activeCommandList->commandList);
#if CAUSTICA_RHI_WITH_AFTERMATH
        if (m_device->isAftermathEnabled())
            m_aftermathTracker.popEvent();
#endif
    }

    void CommandList::setPushConstants(const void* data, size_t byteSize)
    {
        const RootSignature* rootsig = nullptr;
        bool isGraphics = false;

        if (m_currentGraphicsStateValid && m_currentGraphicsState.pipeline)
        {
            GraphicsPipeline* pso = checked_cast<GraphicsPipeline*>(m_currentGraphicsState.pipeline);
            rootsig = pso->rootSignature;
            isGraphics = true;
        }
        else if (m_currentComputeStateValid && m_currentComputeState.pipeline)
        {
            ComputePipeline* pso = checked_cast<ComputePipeline*>(m_currentComputeState.pipeline);
            rootsig = pso->rootSignature;
            isGraphics = false;
        }
        else if (m_currentRayTracingStateValid && m_currentRayTracingState.shaderTable)
        {
            RayTracingPipeline* pso = checked_cast<RayTracingPipeline*>(m_currentRayTracingState.shaderTable->getPipeline());
            rootsig = pso->globalRootSignature;
            isGraphics = false;
        }
        else if (m_currentMeshletStateValid && m_currentMeshletState.pipeline)
        {
            MeshletPipeline* pso = checked_cast<MeshletPipeline*>(m_currentMeshletState.pipeline);
            rootsig = pso->rootSignature;
            isGraphics = true;
        }

        if (!rootsig || !rootsig->pushConstantByteSize)
            return;

        assert(byteSize == rootsig->pushConstantByteSize); // the validation error handles the error message

        if (isGraphics)
            m_activeCommandList->commandList->SetGraphicsRoot32BitConstants(rootsig->rootParameterPushConstants, UINT(byteSize / 4), data, 0);
        else
            m_activeCommandList->commandList->SetComputeRoot32BitConstants(rootsig->rootParameterPushConstants, UINT(byteSize / 4), data, 0);
    }

    bool CommandList::open()
    {
        // Never leave a previous recording instance visible after a failed
        // reopen. A void open() used to let callers continue into the first
        // recording command and crash while dereferencing m_instance.
        m_instance.reset();
        m_activeCommandList.reset();
        m_readyForExecute = false;
        m_recordingFailed = false;

        uint64_t completedInstance = m_queue->updateLastCompletedInstance();

        std::shared_ptr<InternalCommandList> chunk;

        if (!m_commandListPool.empty())
        {
            chunk = m_commandListPool.front();

            if (chunk->lastSubmittedInstance <= completedInstance)
            {
                const HRESULT allocatorHr = chunk->allocator->Reset();
                const HRESULT commandListHr = SUCCEEDED(allocatorHr)
                    ? chunk->commandList->Reset(chunk->allocator, nullptr)
                    : allocatorHr;
                if (FAILED(allocatorHr) || FAILED(commandListHr))
                {
                    std::ostringstream ss;
                    ss << "Failed to reset D3D12 command list, allocatorHRESULT=0x"
                        << std::hex << allocatorHr
                        << ", commandListHRESULT=0x" << commandListHr
                        << ", deviceRemovedReason=0x" << m_context.device->GetDeviceRemovedReason();
                    m_context.messageCallback->message(MessageSeverity::Fatal, ss.str().c_str());
                    return false;
                }
                m_commandListPool.pop_front();
            }
            else
            {
                chunk = nullptr;
            }
        }

        if (chunk == nullptr)
        {
            chunk = createInternalCommandList();
        }

        if (!chunk)
            return false;

        m_activeCommandList = chunk;

        m_instance = std::make_shared<CommandListInstance>();
        m_instance->commandAllocator = m_activeCommandList->allocator;
        m_instance->commandList = m_activeCommandList->commandList;
        m_instance->commandQueue = m_desc.queueType;

        m_recordingVersion = MakeVersion(m_queue->recordingInstance++, m_desc.queueType, false);
        return true;
    }

    void CommandList::clearStateCache()
    {
        m_anyVolatileBufferWrites = false;
        m_currentGraphicsStateValid = false;
        m_currentComputeStateValid = false;
        m_currentMeshletStateValid = false;
        m_currentRayTracingStateValid = false;
        m_currentHeapSRVetc = nullptr;
        m_currentHeapSamplers = nullptr;
        m_currentGraphicsVolatileCBs.resize(0);
        m_currentComputeVolatileCBs.resize(0);
        m_currentSinglePassStereoState = SinglePassStereoState();
    }

    void CommandList::clearState()
    {
        m_activeCommandList->commandList->ClearState(nullptr);

#if CAUSTICA_RHI_D3D12_WITH_NVAPI
        if (m_currentGraphicsStateValid && m_currentSinglePassStereoState.enabled)
        {
            NvAPI_Status Status = NvAPI_D3D12_SetSinglePassStereoMode(m_activeCommandList->commandList,
                1, 0, false);

            if (Status != NVAPI_OK)
            {
                m_context.error("NvAPI_D3D12_SetSinglePassStereoMode call failed");
            }
        }
#endif

        clearStateCache();

        commitDescriptorHeaps();
    }

    void CommandList::close()
    {
        if (!m_instance || !m_activeCommandList || !m_activeCommandList->commandList)
        {
            m_context.messageCallback->message(
                MessageSeverity::Error,
                "Cannot close a D3D12 command list that failed to open");
            return;
        }
        m_stateTracker.keepBufferInitialStates();
        m_stateTracker.keepTextureInitialStates();
        commitBarriers();

#ifdef CAUSTICA_RHI_WITH_ACCEL_STRUCT_MANAGER
        if (!m_instance->accelStructBuildIds.empty())
        {
            m_context.accelStructManager->PopulateCompactionSizeCopiesCommandList(m_activeCommandList->commandList4, m_instance->accelStructBuildIds);
        }
#endif

        const HRESULT closeHr = m_activeCommandList->commandList->Close();
        if (FAILED(closeHr))
        {
            std::ostringstream ss;
            ss << "Failed to close D3D12 command list, HRESULT=0x" << std::hex << closeHr
                << ", deviceRemovedReason=0x" << m_context.device->GetDeviceRemovedReason();
            m_context.messageCallback->message(MessageSeverity::Fatal, ss.str().c_str());
            m_readyForExecute = false;
            return;
        }
        m_readyForExecute = !m_recordingFailed;
        if (m_recordingFailed)
        {
            m_context.messageCallback->message(
                MessageSeverity::Error,
                "D3D12 command list recording failed; submission was rejected");
        }

        clearStateCache();

        m_currentUploadBuffer = nullptr;
        m_volatileConstantBufferAddresses.clear();
        m_uncachedShaderTableStates.clear();
    }

    std::shared_ptr<CommandListInstance> CommandList::executed(Queue* pQueue)
    {
        std::shared_ptr<CommandListInstance> instance = m_instance;
        instance->fence = pQueue->fence;
        instance->submittedInstance = pQueue->lastSubmittedInstance;
        m_instance.reset();
        m_readyForExecute = false;

        m_activeCommandList->lastSubmittedInstance = pQueue->lastSubmittedInstance;
        m_commandListPool.push_back(m_activeCommandList);
        m_activeCommandList.reset();

        for (const auto& it : instance->referencedStagingTextures)
        {
            it->lastUseFence = pQueue->fence;
            it->lastUseFenceValue = instance->submittedInstance;
        }

        for (const auto& it : instance->referencedStagingBuffers)
        {
            it->lastUseFence = pQueue->fence;
            it->lastUseFenceValue = instance->submittedInstance;
        }

        for (const auto& it : instance->referencedTimerQueries)
        {
            it->started = true;
            it->resolved = false;
            it->fence = pQueue->fence;
            it->fenceCounter = instance->submittedInstance;
            it->queue = m_desc.queueType;
        }

        m_stateTracker.commandListSubmitted();

        uint64_t submittedVersion = MakeVersion(instance->submittedInstance, m_desc.queueType, true);
        m_uploadManager.submitChunks(m_recordingVersion, submittedVersion, &instance->referencedUploadChunks);
        m_dxrScratchManager.submitChunks(m_recordingVersion, submittedVersion, &instance->referencedScratchChunks);
        m_recordingVersion = 0;

        return instance;
    }

    void CommandList::convertCoopVecMatrices(coopvec::ConvertMatrixLayoutDesc const* convertDescs, size_t numDescs)
    {
#if CAUSTICA_RHI_D3D12_WITH_COOPVEC
        if (numDescs == 0)
            return;

        if (!m_activeCommandList->commandListPreview)
            return;

        std::vector<D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO> d3dConvertDescs;
        d3dConvertDescs.reserve(numDescs);

        for (size_t i = 0; i < numDescs; ++i)
        {
            coopvec::ConvertMatrixLayoutDesc const& desc = convertDescs[i];
            if (desc.src.buffer == nullptr || desc.dst.buffer == nullptr)
                continue;

            if (m_enableAutomaticBarriers)
            {
                requireBufferState(desc.src.buffer, ResourceStates::ConvertCoopVecMatrixInput);
                requireBufferState(desc.dst.buffer, ResourceStates::ConvertCoopVecMatrixOutput);
                m_bindingStatesDirty = true;
            }

            D3D12_LINEAR_ALGEBRA_MATRIX_CONVERSION_INFO& d3dDesc = d3dConvertDescs.emplace_back();

            d3dDesc.SrcInfo.SrcSize = UINT(desc.src.size);
            d3dDesc.SrcInfo.SrcDataType = convertCoopVecDataType(desc.src.type);
            d3dDesc.SrcInfo.SrcLayout = convertCoopVecMatrixLayout(desc.src.layout);
            d3dDesc.SrcInfo.SrcStride = desc.src.stride != 0
                ? UINT(desc.src.stride)
                : UINT(coopvec::getOptimalMatrixStride(desc.src.type, desc.src.layout, desc.numRows, desc.numColumns));

            d3dDesc.DestInfo.DestSize = UINT(desc.dst.size);
            d3dDesc.DestInfo.DestLayout = convertCoopVecMatrixLayout(desc.dst.layout);
            d3dDesc.DestInfo.DestStride = desc.dst.stride != 0
                ? UINT(desc.dst.stride)
                : UINT(coopvec::getOptimalMatrixStride(desc.dst.type, desc.dst.layout, desc.numRows, desc.numColumns));
            d3dDesc.DestInfo.NumColumns = desc.numColumns;
            d3dDesc.DestInfo.NumRows = desc.numRows;
            d3dDesc.DestInfo.DestDataType = convertCoopVecDataType(desc.dst.type);

            d3dDesc.DataDesc.SrcVA = desc.src.buffer->getGpuVirtualAddress() + desc.src.offset;
            d3dDesc.DataDesc.DestVA = desc.dst.buffer->getGpuVirtualAddress() + desc.dst.offset;
        }

        commitBarriers();

        if (!d3dConvertDescs.empty())
        {
            m_activeCommandList->commandListPreview->ConvertLinearAlgebraMatrix(
                d3dConvertDescs.data(),
                UINT(d3dConvertDescs.size()));
        }
#else
        (void)convertDescs;
        (void)numDescs;
        utils::NotSupported();
#endif
    }
} // namespace caustica::rhi::d3d12

#include "VulkanBackend.h"

namespace caustica::rhi::vulkan
{

    CommandList::CommandList(Device* device, const VulkanContext& context, const CommandListParameters& parameters)
        : m_device(device)
        , m_context(context)
        , m_commandListParameters(parameters)
        , m_stateTracker(context.messageCallback)
        // Vulkan UploadManager does not yet fence-wait on uploadMaxMemory; keep unlimited.
        , m_uploadManager(std::make_unique<UploadManager>(device, parameters.uploadChunkSize, 0, false))
        , m_scratchManager(std::make_unique<UploadManager>(device, parameters.scratchChunkSize, parameters.scratchMaxMemory, true))
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

    caustica::rhi::Object CommandList::getNativeObject(ObjectType objectType)
    {
        switch (objectType)
        {
        case ObjectTypes::VK_CommandBuffer:
            return Object(m_currentCmdBuf->cmdBuf);
        default:
            return nullptr;
        }
    }

    bool CommandList::open()
    {
        m_currentCmdBuf = m_device->getQueue(m_commandListParameters.queueType)->getOrCreateCommandBuffer();
        if (!m_currentCmdBuf)
            return false;

        auto beginInfo = vk::CommandBufferBeginInfo()
            .setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

        (void)m_currentCmdBuf->cmdBuf.begin(&beginInfo);
        m_currentCmdBuf->referencedResources.push_back(this); // prevent deletion of e.g. UploadManager

        clearState();
        return true;
    }

    void CommandList::close()
    {
        endRenderPass();

        m_stateTracker.keepBufferInitialStates();
        m_stateTracker.keepTextureInitialStates();
        commitBarriers();

#ifdef CAUSTICA_RHI_WITH_ACCEL_STRUCT_MANAGER
        if (!m_currentCmdBuf->accelStructBuildIds.empty())
        {
            m_context.accelStructManager->PopulateCompactionSizeCopiesCommandList(m_currentCmdBuf->cmdBuf, m_currentCmdBuf->accelStructBuildIds);
        }
#endif

        m_currentCmdBuf->cmdBuf.end();

        clearState();

        flushVolatileBufferWrites();

        m_uncachedShaderTableStates.clear();
    }

    void CommandList::clearState()
    {
        endRenderPass();

        m_currentPipelineLayout = vk::PipelineLayout();
        m_currentPushConstantsVisibility = vk::ShaderStageFlagBits();

        m_currentGraphicsState = GraphicsState();
        m_currentComputeState = ComputeState();
        m_currentMeshletState = MeshletState();
        m_currentRayTracingState = rt::State();

        m_anyVolatileBufferWrites = false;

        // TODO: add real context clearing code here
    }

    void CommandList::setPushConstants(const void* data, size_t byteSize)
    {
        assert(m_currentCmdBuf);

        m_currentCmdBuf->cmdBuf.pushConstants(m_currentPipelineLayout, m_currentPushConstantsVisibility, 0, uint32_t(byteSize), data);
    }

    void CommandList::executed(Queue& queue, const uint64_t submissionID)
    {
        assert(m_currentCmdBuf);

        m_currentCmdBuf->submissionID = submissionID;

        const CommandQueue queueID = queue.getQueueID();
        const uint64_t recordingID = m_currentCmdBuf->recordingID;

        m_currentCmdBuf = nullptr;

        submitVolatileBuffers(recordingID, submissionID);

        m_stateTracker.commandListSubmitted();

        m_uploadManager->submitChunks(
            MakeVersion(recordingID, queueID, false),
            MakeVersion(submissionID, queueID, true));

        m_scratchManager->submitChunks(
            MakeVersion(recordingID, queueID, false),
            MakeVersion(submissionID, queueID, true));

        m_volatileBufferStates.clear();
    }

    void CommandList::convertCoopVecMatrices(coopvec::ConvertMatrixLayoutDesc const* convertDescs, size_t numDescs)
    {
        if (!m_context.extensions.NV_cooperative_vector)
            return;

        if (numDescs == 0)
            return;

        std::vector<vk::ConvertCooperativeVectorMatrixInfoNV> vkConvertDescs;
        vkConvertDescs.reserve(numDescs);

        std::vector<size_t> dstSizes;
        dstSizes.reserve(numDescs);

        for (size_t i = 0; i < numDescs; i++)
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

            vk::ConvertCooperativeVectorMatrixInfoNV& vkDesc = vkConvertDescs.emplace_back();
            vkDesc.sType = vk::StructureType::eConvertCooperativeVectorMatrixInfoNV;
            vkDesc.srcSize = desc.src.size;
            vkDesc.srcData.deviceAddress = desc.src.buffer->getGpuVirtualAddress() + desc.src.offset;
            vkDesc.pDstSize = &dstSizes.emplace_back(desc.dst.size);
            vkDesc.dstData.deviceAddress = desc.dst.buffer->getGpuVirtualAddress() + desc.dst.offset;
            vkDesc.srcComponentType = convertCoopVecDataType(desc.src.type);
            vkDesc.dstComponentType = convertCoopVecDataType(desc.dst.type);
            vkDesc.numRows = desc.numRows;
            vkDesc.numColumns = desc.numColumns;

            vkDesc.srcLayout = convertCoopVecMatrixLayout(desc.src.layout);
            vkDesc.srcStride = desc.src.stride != 0
                ? desc.src.stride
                : caustica::rhi::coopvec::getOptimalMatrixStride(desc.src.type, desc.src.layout, desc.numRows, desc.numColumns);

            vkDesc.dstLayout = convertCoopVecMatrixLayout(desc.dst.layout);
            vkDesc.dstStride = desc.dst.stride != 0
                ? desc.dst.stride
                : caustica::rhi::coopvec::getOptimalMatrixStride(desc.dst.type, desc.dst.layout, desc.numRows, desc.numColumns);
        }

        commitBarriers();

        if (!vkConvertDescs.empty())
        {
            m_currentCmdBuf->cmdBuf.convertCooperativeVectorMatrixNV(vkConvertDescs);
        }
    }
}

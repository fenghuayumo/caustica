#include "VulkanBackend.h"
#include "rhi/common/misc.h"

namespace caustica::rhi::vulkan
{
    extern vk::ImageAspectFlags guessImageAspectFlags(vk::Format format);

    TrackedCommandBuffer::~TrackedCommandBuffer()
    {
        m_context.device.destroyCommandPool(cmdPool, m_context.allocationCallbacks);
    }

    Queue::Queue(const VulkanContext& context, CommandQueue queueID, vk::Queue queue, uint32_t queueFamilyIndex)
        : m_context(context)
        , m_queue(queue)
        , m_queueID(queueID)
        , m_queueFamilyIndex(queueFamilyIndex)
    {
        auto semaphoreTypeInfo = vk::SemaphoreTypeCreateInfo()
            .setSemaphoreType(vk::SemaphoreType::eTimeline);

        auto semaphoreInfo = vk::SemaphoreCreateInfo()
            .setPNext(&semaphoreTypeInfo);

        trackingSemaphore = context.device.createSemaphore(semaphoreInfo, context.allocationCallbacks);
    }

    Queue::~Queue()
    {
        m_context.device.destroySemaphore(trackingSemaphore, m_context.allocationCallbacks);
        trackingSemaphore = vk::Semaphore();
    }

    TrackedCommandBufferPtr Queue::createCommandBuffer()
    {
        vk::Result res;

        TrackedCommandBufferPtr ret = std::make_shared<TrackedCommandBuffer>(m_context);

        auto cmdPoolInfo = vk::CommandPoolCreateInfo()
                            .setQueueFamilyIndex(m_queueFamilyIndex)
                            .setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer |
                                        vk::CommandPoolCreateFlagBits::eTransient);

        res = m_context.device.createCommandPool(&cmdPoolInfo, m_context.allocationCallbacks, &ret->cmdPool);
        CHECK_VK_FAIL(res)

        // allocate command buffer
        auto allocInfo = vk::CommandBufferAllocateInfo()
                            .setLevel(vk::CommandBufferLevel::ePrimary)
                            .setCommandPool(ret->cmdPool)
                            .setCommandBufferCount(1);

        res = m_context.device.allocateCommandBuffers(&allocInfo, &ret->cmdBuf);
        CHECK_VK_FAIL(res)

        return ret;
    }

    TrackedCommandBufferPtr Queue::getOrCreateCommandBuffer()
    {
        std::lock_guard lockGuard(m_mutex); // this is called from CommandList::open, so free-threaded

        uint64_t recordingID = ++m_lastRecordingID;

        TrackedCommandBufferPtr cmdBuf;
        if (m_commandBuffersPool.empty())
        {
            cmdBuf = createCommandBuffer();
        }
        else
        {
            cmdBuf = m_commandBuffersPool.front();
            m_commandBuffersPool.pop_front();
        }

        cmdBuf->recordingID = recordingID;
        return cmdBuf;
    }

    void Queue::addWaitSemaphore(vk::Semaphore semaphore, uint64_t value)
    {
        if (!semaphore)
            return;

        std::lock_guard lockGuard(m_mutex);
        m_waitSemaphores.push_back(semaphore);
        m_waitSemaphoreValues.push_back(value);
    }

    void Queue::addSignalSemaphore(vk::Semaphore semaphore, uint64_t value)
    {
        if (!semaphore)
            return;

        std::lock_guard lockGuard(m_mutex);
        m_signalSemaphores.push_back(semaphore);
        m_signalSemaphoreValues.push_back(value);
    }

    uint64_t Queue::submit(rhi::CommandList* const* ppCmd, size_t numCmd)
    {
        // Serialize with getOrCreateCommandBuffer / retire / wait-signal staging.
        std::lock_guard lockGuard(m_mutex);

        std::vector<vk::PipelineStageFlags> waitStageArray(m_waitSemaphores.size());
        std::vector<vk::CommandBuffer> commandBuffers(numCmd);

        for (size_t i = 0; i < m_waitSemaphores.size(); i++)
        {
            waitStageArray[i] = vk::PipelineStageFlagBits::eAllCommands;
        }

        const uint64_t submissionID = m_lastSubmittedID + 1;
        const size_t inFlightBefore = m_commandBuffersInFlight.size();

        for (size_t i = 0; i < numCmd; i++)
        {
            CommandList* commandList = checked_cast<CommandList*>(ppCmd[i]);
            TrackedCommandBufferPtr commandBuffer = commandList->getCurrentCmdBuf();

            commandBuffers[i] = commandBuffer->cmdBuf;
            m_commandBuffersInFlight.push_back(commandBuffer);

            for (const auto& buffer : commandBuffer->referencedStagingBuffers)
            {
                buffer->lastUseQueue = m_queueID;
                buffer->lastUseCommandListID = submissionID;
            }
        }

        m_signalSemaphores.push_back(trackingSemaphore);
        m_signalSemaphoreValues.push_back(submissionID);

        auto timelineSemaphoreInfo = vk::TimelineSemaphoreSubmitInfo()
            .setSignalSemaphoreValueCount(uint32_t(m_signalSemaphoreValues.size()))
            .setPSignalSemaphoreValues(m_signalSemaphoreValues.data());

        if (!m_waitSemaphoreValues.empty())
        {
            timelineSemaphoreInfo.setWaitSemaphoreValueCount(uint32_t(m_waitSemaphoreValues.size()));
            timelineSemaphoreInfo.setPWaitSemaphoreValues(m_waitSemaphoreValues.data());
        }

        auto submitInfo = vk::SubmitInfo()
            .setPNext(&timelineSemaphoreInfo)
            .setCommandBufferCount(uint32_t(numCmd))
            .setPCommandBuffers(commandBuffers.data())
            .setWaitSemaphoreCount(uint32_t(m_waitSemaphores.size()))
            .setPWaitSemaphores(m_waitSemaphores.empty() ? nullptr : m_waitSemaphores.data())
            .setPWaitDstStageMask(waitStageArray.data())
            .setSignalSemaphoreCount(uint32_t(m_signalSemaphores.size()))
            .setPSignalSemaphores(m_signalSemaphores.empty() ? nullptr : m_signalSemaphores.data());

        try {
            m_queue.submit(submitInfo);
        }
        catch (vk::DeviceLostError&)
        {
            m_context.messageCallback->message(MessageSeverity::Error, "Device Removed!");
            while (m_commandBuffersInFlight.size() > inFlightBefore)
                m_commandBuffersInFlight.pop_back();
            m_waitSemaphores.clear();
            m_waitSemaphoreValues.clear();
            m_signalSemaphores.clear();
            m_signalSemaphoreValues.clear();
            return 0;
        }

        m_lastSubmittedID = submissionID;
        m_waitSemaphores.clear();
        m_waitSemaphoreValues.clear();
        m_signalSemaphores.clear();
        m_signalSemaphoreValues.clear();

        return m_lastSubmittedID;
    }

    void Queue::updateTextureTileMappings(rhi::Texture* _texture, const TextureTilesMapping* tileMappings, uint32_t numTileMappings)
    {
        Texture* texture = checked_cast<Texture*>(_texture);

        std::vector<vk::SparseImageMemoryBind> sparseImageMemoryBinds;
        std::vector<vk::SparseMemoryBind> sparseMemoryBinds;

        vk::ImageCreateInfo& imageInfo = texture->imageInfo;
		vk::ImageAspectFlags textureAspectFlags = guessImageAspectFlags(imageInfo.format);

		// Required for extent and offset since they must be multiples of the tile dimensions
		uint32_t tileWidth = 1;
		uint32_t tileHeight = 1;
		uint32_t tileDepth = 1;

        // Mip tail info, required for resource offset
        vk::DeviceSize imageMipTailOffset = 0;

        std::vector<vk::SparseImageFormatProperties> formatProperties = m_context.physicalDevice.getSparseImageFormatProperties(imageInfo.format, imageInfo.imageType, imageInfo.samples, imageInfo.usage, imageInfo.tiling);
		std::vector<vk::SparseImageMemoryRequirements> memoryRequirements = m_context.device.getImageSparseMemoryRequirements(texture->image);

		if (!formatProperties.empty())
		{
			tileWidth = formatProperties[0].imageGranularity.width;
			tileHeight = formatProperties[0].imageGranularity.height;
			tileDepth = formatProperties[0].imageGranularity.depth;
		}

        if (!memoryRequirements.empty())
        {
			imageMipTailOffset = memoryRequirements[0].imageMipTailOffset;
        }

        for (size_t i = 0; i < numTileMappings; i++)
        {
            uint32_t numRegions = tileMappings[i].numTextureRegions;
            Heap* heap = tileMappings[i].heap ? checked_cast<Heap*>(tileMappings[i].heap) : nullptr;
            vk::DeviceMemory deviceMemory = heap ? heap->memory : VK_NULL_HANDLE;

            for (uint32_t j = 0; j < numRegions; ++j)
            {
                const TiledTextureCoordinate& tiledTextureCoordinate = tileMappings[i].tiledTextureCoordinates[j];
                const TiledTextureRegion& tiledTextureRegion = tileMappings[i].tiledTextureRegions[j];

                if (tiledTextureRegion.tilesNum)
                {
                    sparseMemoryBinds.push_back(vk::SparseMemoryBind()
                        .setResourceOffset(imageMipTailOffset + tiledTextureCoordinate.arrayLevel * imageMipTailOffset)
                        .setSize(tiledTextureRegion.tilesNum * texture->tileByteSize)
                        .setMemory(deviceMemory)
                        .setMemoryOffset(deviceMemory ? tileMappings[i].byteOffsets[j] : 0));
                }
                else
                {
                    vk::ImageSubresource subresource = {};
                    subresource.arrayLayer = tiledTextureCoordinate.arrayLevel;
                    subresource.mipLevel = tiledTextureCoordinate.mipLevel;
					subresource.aspectMask = textureAspectFlags; // Required for sparse binding

                    vk::Offset3D offset3D;
                    offset3D.x = tiledTextureCoordinate.x * tileWidth;
                    offset3D.y = tiledTextureCoordinate.y * tileHeight;
                    offset3D.z = tiledTextureCoordinate.z * tileHeight;

                    vk::Extent3D extent3D;
                    extent3D.width = tiledTextureRegion.width * tileWidth;
                    extent3D.height = tiledTextureRegion.height * tileHeight;
                    extent3D.depth = tiledTextureRegion.depth * tileDepth;

                    sparseImageMemoryBinds.push_back(vk::SparseImageMemoryBind()
                        .setSubresource(subresource)
                        .setOffset(offset3D)
                        .setExtent(extent3D)
                        .setMemory(deviceMemory)
                        .setMemoryOffset(deviceMemory ? tileMappings[i].byteOffsets[j] : 0));
                }
            }
        }

        vk::BindSparseInfo bindSparseInfo = {};

        vk::SparseImageMemoryBindInfo sparseImageMemoryBindInfo;
        if (!sparseImageMemoryBinds.empty())
        {
            sparseImageMemoryBindInfo.setImage(texture->image);
            sparseImageMemoryBindInfo.setBinds(sparseImageMemoryBinds);
            bindSparseInfo.setImageBinds(sparseImageMemoryBindInfo);
        }

        vk::SparseImageOpaqueMemoryBindInfo sparseImageOpaqueMemoryBindInfo;
        if (!sparseMemoryBinds.empty())
        {
            sparseImageOpaqueMemoryBindInfo.setImage(texture->image);
            sparseImageOpaqueMemoryBindInfo.setBinds(sparseMemoryBinds);
            bindSparseInfo.setImageOpaqueBinds(sparseImageOpaqueMemoryBindInfo);
        }

        m_queue.bindSparse(bindSparseInfo, vk::Fence());
    }

    uint64_t Queue::updateLastFinishedID()
    {
        m_lastFinishedID = m_context.device.getSemaphoreCounterValue(trackingSemaphore);

        return m_lastFinishedID;
    }

    void Queue::retireCommandBuffers()
    {
        std::lock_guard lockGuard(m_mutex);

        std::list<TrackedCommandBufferPtr> submissions = std::move(m_commandBuffersInFlight);

        uint64_t lastFinishedID = updateLastFinishedID();

        for (const TrackedCommandBufferPtr& cmd : submissions)
        {
            if (cmd->submissionID <= lastFinishedID)
            {
                cmd->referencedResources.clear();
                cmd->referencedStagingBuffers.clear();
                cmd->submissionID = 0;
                m_commandBuffersPool.push_back(cmd);

#ifdef CAUSTICA_RHI_WITH_ACCEL_STRUCT_MANAGER
                if (!cmd->accelStructBuildIds.empty())
                {
                    std::lock_guard accelStructLockGuard(m_context.accelStructResources->asListMutex);

                    m_context.accelStructResources->asBuildsCompleted.insert(m_context.accelStructResources->asBuildsCompleted.end(),
                        cmd->accelStructBuildIds.begin(), cmd->accelStructBuildIds.end());

                    cmd->accelStructBuildIds.clear();
                }
                if (!cmd->accelStructCompactionIds.empty())
                {
                    m_context.accelStructManager->GarbageCollection(cmd->accelStructCompactionIds);
                    cmd->accelStructCompactionIds.clear();
                }
#endif
            }
            else
            {
                m_commandBuffersInFlight.push_back(cmd);
            }
        }
    }

    TrackedCommandBufferPtr Queue::getCommandBufferInFlight(uint64_t submissionID)
    {
        for (const TrackedCommandBufferPtr& cmd : m_commandBuffersInFlight)
        {
            if (cmd->submissionID == submissionID)
                return cmd;
        }

        return nullptr;
    }

    VkSemaphore Device::getQueueSemaphore(CommandQueue queueID)
    {
        Queue& queue = *m_queues[uint32_t(queueID)];

        return queue.trackingSemaphore;
    }

    void Device::queueWaitForSemaphore(CommandQueue waitQueueID, VkSemaphore semaphore, uint64_t value)
    {
        Queue& waitQueue = *m_queues[uint32_t(waitQueueID)];

        waitQueue.addWaitSemaphore(semaphore, value);
    }

    void Device::queueSignalSemaphore(CommandQueue executionQueueID, VkSemaphore semaphore, uint64_t value)
    {
        Queue& executionQueue = *m_queues[uint32_t(executionQueueID)];

        executionQueue.addSignalSemaphore(semaphore, value);
    }

    void Device::queueWaitForCommandList(CommandQueue waitQueueID, CommandQueue executionQueueID, uint64_t instance)
    {
        queueWaitForSemaphore(waitQueueID, getQueueSemaphore(executionQueueID), instance);
    }

    void Device::updateTextureTileMappings(rhi::Texture* texture, const TextureTilesMapping* tileMappings, uint32_t numTileMappings, CommandQueue executionQueue)
    {
        Queue& queue = *m_queues[uint32_t(executionQueue)];

        queue.updateTextureTileMappings(texture, tileMappings, numTileMappings);
    }

    uint64_t Device::queueGetCompletedInstance(CommandQueue queue)
    {
        return m_context.device.getSemaphoreCounterValue(getQueueSemaphore(queue));
    }

    bool Queue::pollCommandList(uint64_t commandListID)
    {
        if (commandListID > m_lastSubmittedID || commandListID == 0)
            return false;

        bool completed = getLastFinishedID() >= commandListID;
        if (completed)
            return true;

        completed = updateLastFinishedID() >= commandListID;
        return completed;
    }

    bool Queue::waitCommandList(uint64_t commandListID, uint64_t timeout)
    {
        if (commandListID > m_lastSubmittedID || commandListID == 0)
            return false;

        if (pollCommandList(commandListID))
            return true;

        std::array<const vk::Semaphore, 1> semaphores = { trackingSemaphore };
        std::array<uint64_t, 1> waitValues = { commandListID };

        auto waitInfo = vk::SemaphoreWaitInfo()
            .setSemaphores(semaphores)
            .setValues(waitValues);

        vk::Result result = m_context.device.waitSemaphores(waitInfo, timeout);

        return (result == vk::Result::eSuccess);
    }
} // namespace caustica::rhi::vulkan

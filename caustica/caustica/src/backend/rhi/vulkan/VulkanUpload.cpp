#include "VulkanBackend.h"
#include <rhi/common/misc.h>

namespace caustica::rhi::vulkan
{

    std::shared_ptr<BufferChunk> UploadManager::CreateChunk(uint64_t size)
    {
        std::shared_ptr<BufferChunk> chunk = std::make_shared<BufferChunk>();

        if (m_isScratchBuffer)
        {
            BufferDesc desc;
            desc.byteSize = size;
            desc.cpuAccess = CpuAccessMode::None;
            desc.debugName = "ScratchBufferChunk";
            desc.canHaveUAVs = true;

            chunk->buffer = m_device->createBuffer(desc);
            chunk->mappedMemory = nullptr;
            chunk->bufferSize = size;
        }
        else
        {
            BufferDesc desc;
            desc.byteSize = size;
            desc.cpuAccess = CpuAccessMode::Write;
            desc.debugName = "UploadChunk";

            // The upload manager buffers are used in buildTopLevelAccelStruct to store instance data, and SBT for shader entries
            desc.isAccelStructBuildInput = m_device->queryFeatureSupport(Feature::RayTracingAccelStruct);
            desc.isShaderBindingTable = m_device->queryFeatureSupport(Feature::RayTracingAccelStruct);

            chunk->buffer = m_device->createBuffer(desc);
            chunk->mappedMemory = m_device->mapBuffer(chunk->buffer, CpuAccessMode::Write);
            chunk->bufferSize = size;
        }

        return chunk;
    }

    bool UploadManager::suballocateBuffer(uint64_t size, Buffer** pBuffer, uint64_t* pOffset, void** pCpuVA,
        uint64_t currentVersion, uint32_t alignment)
    {
        std::shared_ptr<BufferChunk> chunkToRetire;

        if (m_currentChunk)
        {
            uint64_t alignedOffset = align(m_currentChunk->writePointer, (uint64_t)alignment);
            uint64_t endOfDataInChunk = alignedOffset + size;

            if (endOfDataInChunk <= m_currentChunk->bufferSize)
            {
                m_currentChunk->writePointer = endOfDataInChunk;

                *pBuffer = checked_cast<Buffer*>(m_currentChunk->buffer.Get());
                *pOffset = alignedOffset;
                if (pCpuVA && m_currentChunk->mappedMemory)
                    *pCpuVA = (char*)m_currentChunk->mappedMemory + alignedOffset;

                return true;
            }

            chunkToRetire = m_currentChunk;
            m_currentChunk.reset();
        }

        CommandQueue queue = VersionGetQueue(currentVersion);
        uint64_t completedInstance = m_device->queueGetCompletedInstance(queue);

        for (auto it = m_chunkPool.begin(); it != m_chunkPool.end(); ++it)
        {
            std::shared_ptr<BufferChunk> chunk = *it;

            if (VersionGetSubmitted(chunk->version)
                && VersionGetInstance(chunk->version) <= completedInstance)
            {
                chunk->version = 0;
            }

            if (chunk->version == 0 && chunk->bufferSize >= size)
            {
                m_chunkPool.erase(it);
                m_currentChunk = chunk;
                break;
            }
        }

        if (chunkToRetire)
        {
            m_chunkPool.push_back(chunkToRetire);
        }

        if (!m_currentChunk)
        {
            uint64_t sizeToAllocate = align(std::max(size, m_defaultChunkSize), BufferChunk::c_sizeAlignment);

            if ((m_memoryLimit > 0) && (m_allocatedMemory + sizeToAllocate > m_memoryLimit))
                return false;

            m_currentChunk = CreateChunk(sizeToAllocate);
        }

        m_currentChunk->version = currentVersion;
        m_currentChunk->writePointer = size;

        *pBuffer = checked_cast<Buffer*>(m_currentChunk->buffer.Get());
        *pOffset = 0;
        if (pCpuVA)
            *pCpuVA = m_currentChunk->mappedMemory;

        return true;
    }

    void UploadManager::submitChunks(uint64_t currentVersion, uint64_t submittedVersion)
    {
        if (m_currentChunk)
        {
            m_chunkPool.push_back(m_currentChunk);
            m_currentChunk.reset();
        }

        for (const auto& chunk : m_chunkPool)
        {
            if (chunk->version == currentVersion)
                chunk->version = submittedVersion;
        }
    }

}
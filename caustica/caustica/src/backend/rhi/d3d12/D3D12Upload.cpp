#include "D3D12Backend.h"

#include <rhi/common/misc.h>
#include <algorithm>
#include <sstream>

namespace caustica::rhi::d3d12
{

    BufferChunk::~BufferChunk()
    {
        if (buffer && cpuVA)
        {
            buffer->Unmap(0, nullptr);
            cpuVA = nullptr;
        }
    }

    UploadManager::UploadManager(const Context& context, class Queue* pQueue, size_t defaultChunkSize, uint64_t memoryLimit, bool isScratchBuffer)
        : m_context(context)
        , m_queue(pQueue)
        , m_defaultChunkSize(defaultChunkSize)
        , m_memoryLimit(memoryLimit)
        , m_isScratchBuffer(isScratchBuffer)
    {
        assert(pQueue);
    }

    std::shared_ptr<BufferChunk> UploadManager::createChunk(size_t size)
    {
        auto chunk = std::make_shared<BufferChunk>();

        size = align(size, BufferChunk::c_sizeAlignment);

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = m_isScratchBuffer ? D3D12_HEAP_TYPE_DEFAULT : D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = size;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (m_isScratchBuffer) bufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        HRESULT hr = m_context.device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            m_isScratchBuffer ? D3D12_RESOURCE_STATE_COMMON: D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&chunk->buffer));

        if (FAILED(hr))
        {
            std::stringstream ss;
            ss << "CreateCommittedResource failed for "
                << (m_isScratchBuffer ? "DXR scratch" : "upload")
                << " chunk, size=" << size
                << ", HRESULT=0x" << std::hex << hr;
            m_context.error(ss.str());
            return nullptr;
        }

        if (!m_isScratchBuffer)
        {
            hr = chunk->buffer->Map(0, nullptr, &chunk->cpuVA);

            if (FAILED(hr))
            {
                std::stringstream ss;
                ss << "Map failed for upload chunk, size=" << size
                    << ", HRESULT=0x" << std::hex << hr;
                m_context.error(ss.str());
                return nullptr;
            }
        }

        chunk->bufferSize = size;
        chunk->gpuVA = chunk->buffer->GetGPUVirtualAddress();
        chunk->identifier = uint32_t(m_chunkPool.size());
        m_allocatedMemory += size;

        std::wstringstream wss;
        if (m_isScratchBuffer)
            wss << L"DXR Scratch Buffer " << chunk->identifier;
        else
            wss << L"Upload Buffer " << chunk->identifier;
        chunk->buffer->SetName(wss.str().c_str());

        return chunk;
    }

    // Wait for a submitted chunk's fence, then reuse it. Scratch needs a UAV barrier;
    // UPLOAD heaps are CPU-writable after the copy has completed on the GPU.
    bool UploadManager::acquireReusableChunk(size_t sizeToAllocate, ID3D12GraphicsCommandList* pCommandList)
    {
        std::shared_ptr<BufferChunk> bestChunk;
        for (const auto& candidateChunk : m_chunkPool)
        {
            if (candidateChunk->bufferSize < sizeToAllocate)
                continue;

            if (!bestChunk)
            {
                bestChunk = candidateChunk;
                continue;
            }

            const bool candidateSubmitted = VersionGetSubmitted(candidateChunk->version);
            const bool bestSubmitted = VersionGetSubmitted(bestChunk->version);
            const uint64_t candidateInstance = VersionGetInstance(candidateChunk->version);
            const uint64_t bestInstance = VersionGetInstance(bestChunk->version);

            if ((candidateSubmitted && !bestSubmitted) ||
                (candidateSubmitted == bestSubmitted && candidateInstance < bestInstance) ||
                (candidateSubmitted == bestSubmitted && candidateInstance == bestInstance
                    && candidateChunk->bufferSize > bestChunk->bufferSize))
            {
                bestChunk = candidateChunk;
            }
        }

        if (!bestChunk)
            return false;

        // Still being recorded into the current CL - cannot reclaim until submit.
        if (!VersionGetSubmitted(bestChunk->version) && bestChunk->version != 0)
            return false;

        if (VersionGetSubmitted(bestChunk->version))
        {
            uint64_t latestCompletedInstance = m_queue->updateLastCompletedInstance();
            const uint64_t chunkInstance = VersionGetInstance(bestChunk->version);
            if (chunkInstance > latestCompletedInstance)
            {
                HANDLE waitEvent = CreateEvent(nullptr, false, false, nullptr);
                if (!waitEvent)
                    return false;

                if (!WaitForFence(m_queue->fence, chunkInstance, waitEvent))
                {
                    CloseHandle(waitEvent);
                    return false;
                }
                CloseHandle(waitEvent);
                m_queue->lastCompletedInstance = std::max(m_queue->lastCompletedInstance, chunkInstance);
            }
        }

        m_chunkPool.erase(std::find(m_chunkPool.begin(), m_chunkPool.end(), bestChunk));
        m_currentChunk = bestChunk;
        bestChunk->version = 0;

        if (m_isScratchBuffer)
        {
            assert(pCommandList);
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = bestChunk->buffer;
            pCommandList->ResourceBarrier(1, &barrier);
        }

        return true;
    }

    bool UploadManager::suballocateBuffer(uint64_t size, ID3D12GraphicsCommandList* pCommandList, ID3D12Resource** pBuffer, size_t* pOffset,
        void** pCpuVA, D3D12_GPU_VIRTUAL_ADDRESS* pGpuVA, uint64_t currentVersion, uint32_t alignment)
    {
        // Scratch allocations need a command list, upload ones don't
        assert(!m_isScratchBuffer || pCommandList);

        std::shared_ptr<BufferChunk> chunkToRetire;

        // Try to allocate from the current chunk first
        if (m_currentChunk != nullptr)
        {
            uint64_t alignedOffset = align(m_currentChunk->writePointer, (uint64_t)alignment);
            uint64_t endOfDataInChunk = alignedOffset + size;

            if (endOfDataInChunk <= m_currentChunk->bufferSize)
            {
                // The buffer can fit into the current chunk - great, we're done
                m_currentChunk->writePointer = endOfDataInChunk;

                if (pBuffer) *pBuffer = m_currentChunk->buffer;
                if (pOffset) *pOffset = alignedOffset;
                if (pCpuVA && m_currentChunk->cpuVA)
                    *pCpuVA = (char*)m_currentChunk->cpuVA + alignedOffset;
                if (pGpuVA && m_currentChunk->gpuVA)
                    *pGpuVA = m_currentChunk->gpuVA + alignedOffset;

                return true;
            }

            chunkToRetire = m_currentChunk;
            m_currentChunk.reset();
        }

        uint64_t completedInstance = m_queue->lastCompletedInstance;

        // Try to find a chunk in the pool that's no longer used and is large enough to allocate our buffer
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

            const bool overBudget = (m_memoryLimit > 0) && (m_allocatedMemory + sizeToAllocate > m_memoryLimit);
            if (overBudget)
            {
                if (!acquireReusableChunk(sizeToAllocate, pCommandList))
                    return false;
            }
            else
            {
                m_currentChunk = createChunk(sizeToAllocate);
                if (!m_currentChunk)
                {
                    // Driver OOM: fall back to waiting on an in-flight chunk.
                    if (!acquireReusableChunk(sizeToAllocate, pCommandList))
                        return false;
                }
            }
        }

        m_currentChunk->version = currentVersion;
        m_currentChunk->writePointer = size;

        if (pBuffer) *pBuffer = m_currentChunk->buffer;
        if (pOffset) *pOffset = 0;
        if (pCpuVA) *pCpuVA = m_currentChunk->cpuVA;
        if (pGpuVA) *pGpuVA = m_currentChunk->gpuVA;

        return true;
    }

    void UploadManager::submitChunks(
        uint64_t currentVersion,
        uint64_t submittedVersion,
        std::vector<std::shared_ptr<BufferChunk>>* referencedChunks)
    {
        if (m_currentChunk)
        {
            m_chunkPool.push_back(m_currentChunk);
            m_currentChunk.reset();
        }

        for (const auto& chunk : m_chunkPool)
        {
            if (chunk->version == currentVersion)
            {
                chunk->version = submittedVersion;
                if (referencedChunks)
                    referencedChunks->push_back(chunk);
            }
        }
    }
} // namespace caustica::rhi::d3d12

#include <render/graph/TransientResourceAllocator.h>
#include <render/graph/RenderBufferPool.h>
#include <render/graph/RenderTargetPool.h>

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstddef>

namespace caustica::rg
{

namespace
{
    struct TransientAllocationRequest
    {
        size_t resourceIndex = 0;
        int32_t firstPassOrder = INT32_MAX;
        int32_t lastPassOrder = -1;
        uint64_t exactDescHash = 0;
        uint64_t compatibilityHash = 0;
    };

    struct PhysicalTextureSlot
    {
        nvrhi::TextureHandle handle;
        TextureDesc desc{};
        uint64_t exactDescHash = 0;
        int32_t lastPassOrder = -1;
    };

    struct PhysicalBufferSlot
    {
        nvrhi::BufferHandle handle;
        uint64_t compatibilityHash = 0;
        uint64_t byteSize = 0;
        int32_t lastPassOrder = -1;
    };

    struct HeapMemorySlot
    {
        uint64_t offset = 0;
        uint64_t size = 0;
        uint64_t alignment = 1;
        size_t resourceIndex = SIZE_MAX;
        int32_t lastPassOrder = -1;
    };

    struct TextureHeapRequest
    {
        size_t resourceIndex = 0;
        int32_t firstPassOrder = INT32_MAX;
        int32_t lastPassOrder = -1;
        nvrhi::MemoryRequirements memReq{};
        nvrhi::TextureHandle virtualTexture;
        uint64_t heapOffset = 0;
    };

    struct BufferHeapRequest
    {
        size_t resourceIndex = 0;
        int32_t firstPassOrder = INT32_MAX;
        int32_t lastPassOrder = -1;
        nvrhi::MemoryRequirements memReq{};
        nvrhi::BufferHandle virtualBuffer;
        uint64_t heapOffset = 0;
    };

    int findAliasedTextureSlot(
        const std::vector<PhysicalTextureSlot>& physicalTextures,
        uint64_t exactDescHash,
        const TextureDesc& desc,
        int32_t firstPassOrder)
    {
        int exactMatch = -1;
        int supersetMatch = -1;
        uint64_t supersetArea = UINT64_MAX;

        for (int slotIndex = 0; slotIndex < static_cast<int>(physicalTextures.size()); ++slotIndex)
        {
            const PhysicalTextureSlot& slot = physicalTextures[static_cast<size_t>(slotIndex)];
            if (slot.lastPassOrder >= firstPassOrder)
                continue;

            if (slot.exactDescHash == exactDescHash)
            {
                exactMatch = slotIndex;
                break;
            }

            if (!textureDescCovers(slot.desc, desc))
                continue;

            const uint64_t slotArea = static_cast<uint64_t>(slot.desc.width) * slot.desc.height * slot.desc.depth;
            if (slotArea < supersetArea)
            {
                supersetArea = slotArea;
                supersetMatch = slotIndex;
            }
        }

        return exactMatch >= 0 ? exactMatch : supersetMatch;
    }

    int findAliasedBufferSlot(
        const std::vector<PhysicalBufferSlot>& physicalBuffers,
        uint64_t compatibilityHash,
        uint64_t byteSize,
        int32_t firstPassOrder)
    {
        int bestSlot = -1;
        uint64_t bestSize = UINT64_MAX;

        for (int slotIndex = 0; slotIndex < static_cast<int>(physicalBuffers.size()); ++slotIndex)
        {
            const PhysicalBufferSlot& slot = physicalBuffers[static_cast<size_t>(slotIndex)];
            if (slot.compatibilityHash != compatibilityHash)
                continue;
            if (slot.byteSize < byteSize)
                continue;
            if (slot.lastPassOrder >= firstPassOrder)
                continue;

            if (slot.byteSize < bestSize)
            {
                bestSize = slot.byteSize;
                bestSlot = slotIndex;
            }
        }

        return bestSlot;
    }

    int findHeapMemorySlot(
        const std::vector<HeapMemorySlot>& memorySlots,
        const nvrhi::MemoryRequirements& memReq,
        int32_t firstPassOrder)
    {
        int bestSlot = -1;
        uint64_t bestSize = UINT64_MAX;

        for (int slotIndex = 0; slotIndex < static_cast<int>(memorySlots.size()); ++slotIndex)
        {
            const HeapMemorySlot& slot = memorySlots[static_cast<size_t>(slotIndex)];
            if (slot.size < memReq.size)
                continue;
            if (slot.lastPassOrder >= firstPassOrder)
                continue;
            if (slot.offset % std::max<uint64_t>(memReq.alignment, 1) != 0)
                continue;

            if (slot.size < bestSize)
            {
                bestSize = slot.size;
                bestSlot = slotIndex;
            }
        }

        return bestSlot;
    }

    TextureDesc textureDescFromNative(nvrhi::ITexture* texture, const TextureDesc& requestDesc)
    {
        TextureDesc desc = requestDesc;
        if (!texture)
            return desc;

        const nvrhi::TextureDesc& nativeDesc = texture->getDesc();
        desc.width = nativeDesc.width;
        desc.height = nativeDesc.height;
        desc.depth = nativeDesc.depth;
        desc.mipLevels = nativeDesc.mipLevels;
        desc.arraySize = nativeDesc.arraySize;
        desc.format = fromNvrhiFormat(nativeDesc.format);
        return desc;
    }

    uint64_t alignUp(uint64_t value, uint64_t alignment)
    {
        if (alignment == 0)
            return value;
        return (value + alignment - 1) / alignment * alignment;
    }

}

void TransientResourceAllocator::allocate(
    GraphBuilder& graph,
    const std::vector<bool>& referencedTextures,
    const std::vector<bool>& referencedBuffers,
    const std::vector<GraphBuilder::TransientLifetime>& textureLifetimes,
    const std::vector<GraphBuilder::TransientLifetime>& bufferLifetimes)
{
    assert(graph.m_device);
    graph.m_transientHeaps.clear();

    const auto updateHeapPoolStats = [&graph]() {
        graph.m_transientStats.pooledHeapCount = 0;
        graph.m_transientStats.pooledHeapBytes = 0;

        for (const nvrhi::HeapHandle& heap : graph.m_transientHeapPool)
        {
            if (!heap)
                continue;
            ++graph.m_transientStats.pooledHeapCount;
            graph.m_transientStats.pooledHeapBytes += heap->getDesc().capacity;
        }
    };

    const auto acquireHeap = [&graph](uint64_t capacity, const char* debugName) -> nvrhi::HeapHandle {
        int bestSlot = -1;
        uint64_t bestCapacity = UINT64_MAX;

        for (int i = 0; i < static_cast<int>(graph.m_transientHeapPool.size()); ++i)
        {
            nvrhi::HeapHandle& heap = graph.m_transientHeapPool[static_cast<size_t>(i)];
            if (!heap)
                continue;
            const nvrhi::HeapDesc& desc = heap->getDesc();
            if (desc.type != nvrhi::HeapType::DeviceLocal || desc.capacity < capacity)
                continue;
            if (desc.capacity < bestCapacity)
            {
                bestCapacity = desc.capacity;
                bestSlot = i;
            }
        }

        if (bestSlot >= 0)
        {
            nvrhi::HeapHandle heap = graph.m_transientHeapPool[static_cast<size_t>(bestSlot)];
            graph.m_transientHeapPool.erase(graph.m_transientHeapPool.begin() + bestSlot);
            ++graph.m_transientStats.reusedHeapCount;
            return heap;
        }

        nvrhi::HeapDesc heapDesc;
        heapDesc.type = nvrhi::HeapType::DeviceLocal;
        heapDesc.capacity = capacity;
        heapDesc.debugName = debugName;
        ++graph.m_transientStats.createdHeapCount;
        return graph.m_device->createHeap(heapDesc);
    };

    updateHeapPoolStats();

    std::vector<TransientAllocationRequest> textureRequests;
    textureRequests.reserve(graph.m_textures.size());
    for (size_t i = 0; i < graph.m_textures.size(); ++i)
    {
        if (i >= referencedTextures.size() || !referencedTextures[i])
            continue;
        if (graph.m_textures[i].lifetime != GraphBuilder::ResourceLifetime::Transient)
            continue;
        if (i >= textureLifetimes.size() || textureLifetimes[i].lastPassOrder < 0)
            continue;

        const TextureDesc& desc = graph.m_textures[i].desc;
        textureRequests.push_back({
            i,
            textureLifetimes[i].firstPassOrder,
            textureLifetimes[i].lastPassOrder,
            hashTextureDesc(desc),
            hashTextureCompatibilityDesc(desc),
        });
    }
    graph.m_transientStats.transientTextureCount = static_cast<uint32_t>(textureRequests.size());

    std::sort(textureRequests.begin(), textureRequests.end(), [](const TransientAllocationRequest& a, const TransientAllocationRequest& b) {
        if (a.firstPassOrder != b.firstPassOrder)
            return a.firstPassOrder < b.firstPassOrder;
        return a.resourceIndex < b.resourceIndex;
    });

    std::vector<PhysicalTextureSlot> physicalTextures;
    std::vector<TextureHeapRequest> heapRequests;
    const bool canUseVirtualResources = graph.m_device->queryFeatureSupport(nvrhi::Feature::VirtualResources);

    for (const TransientAllocationRequest& request : textureRequests)
    {
        GraphBuilder::GraphTexture& resource = graph.m_textures[request.resourceIndex];
        const int physicalIndex = findAliasedTextureSlot(
            physicalTextures,
            request.exactDescHash,
            resource.desc,
            request.firstPassOrder);

        if (physicalIndex >= 0)
        {
            PhysicalTextureSlot& slot = physicalTextures[static_cast<size_t>(physicalIndex)];
            slot.lastPassOrder = std::max(slot.lastPassOrder, request.lastPassOrder);
            resource.owned = slot.handle;
            resource.texture = slot.handle;
            resource.currentState = nvrhi::ResourceStates::Common;
            ++graph.m_transientStats.aliasedTextureCount;
            continue;
        }

        if (graph.m_renderTargetPool)
        {
            if (nvrhi::TextureHandle pooled = graph.m_renderTargetPool->tryAcquireTexture(resource.desc))
            {
                PhysicalTextureSlot slot{};
                slot.desc = textureDescFromNative(pooled, resource.desc);
                slot.exactDescHash = hashTextureDesc(resource.desc);
                slot.handle = pooled;
                slot.lastPassOrder = request.lastPassOrder;
                physicalTextures.push_back(slot);

                resource.owned = slot.handle;
                resource.texture = slot.handle;
                resource.currentState = nvrhi::ResourceStates::Common;
                ++graph.m_transientStats.pooledTextureCount;
                continue;
            }
        }

        if (!canUseVirtualResources)
        {
            PhysicalTextureSlot slot{};
            slot.desc = resource.desc;
            slot.exactDescHash = request.exactDescHash;
            slot.handle = graph.m_renderTargetPool
                ? graph.m_renderTargetPool->acquireTexture(resource.desc)
                : graph.createNativeTexture(resource.desc);
            slot.lastPassOrder = request.lastPassOrder;
            physicalTextures.push_back(slot);

            resource.owned = slot.handle;
            resource.texture = slot.handle;
            resource.currentState = nvrhi::ResourceStates::Common;
            if (graph.m_renderTargetPool)
                ++graph.m_transientStats.pooledTextureCount;
            continue;
        }

        nvrhi::TextureHandle virtualTexture = graph.createNativeTexture(resource.desc, true);
        assert(virtualTexture);
        const nvrhi::MemoryRequirements memReq = graph.m_device->getTextureMemoryRequirements(virtualTexture);
        heapRequests.push_back({
            request.resourceIndex,
            request.firstPassOrder,
            request.lastPassOrder,
            memReq,
            virtualTexture,
        });
    }

    if (!heapRequests.empty())
    {
        std::vector<HeapMemorySlot> memorySlots;
        uint64_t heapSize = 0;

        for (TextureHeapRequest& request : heapRequests)
        {
            const int slotIndex = findHeapMemorySlot(memorySlots, request.memReq, request.firstPassOrder);
            if (slotIndex >= 0)
            {
                HeapMemorySlot& slot = memorySlots[static_cast<size_t>(slotIndex)];
                if (slot.resourceIndex != SIZE_MAX)
                {
                    graph.m_textureAliasingBarriers.push_back({
                        TextureHandle{ static_cast<uint32_t>(slot.resourceIndex) },
                        TextureHandle{ static_cast<uint32_t>(request.resourceIndex) },
                    });
                    ++graph.m_transientStats.aliasedTextureCount;
                }
                slot.lastPassOrder = std::max(slot.lastPassOrder, request.lastPassOrder);
                slot.resourceIndex = request.resourceIndex;
                request.heapOffset = slot.offset;
                continue;
            }

            const uint64_t alignment = std::max<uint64_t>(request.memReq.alignment, 1);
            const uint64_t alignedOffset = alignUp(heapSize, alignment);
            heapSize = alignedOffset + request.memReq.size;
            memorySlots.push_back({ alignedOffset, request.memReq.size, alignment, request.resourceIndex, request.lastPassOrder });
            request.heapOffset = alignedOffset;
        }

        nvrhi::HeapHandle heap = acquireHeap(heapSize, "rg_transient_texture_heap");
        assert(heap);
        graph.m_transientHeaps.push_back(heap);
        graph.m_transientStats.textureHeapBytes += heap->getDesc().capacity;
        graph.m_transientStats.placedTextureCount += static_cast<uint32_t>(heapRequests.size());

        for (const TextureHeapRequest& request : heapRequests)
        {
            GraphBuilder::GraphTexture& resource = graph.m_textures[request.resourceIndex];
            const bool bound = graph.m_device->bindTextureMemory(request.virtualTexture, heap, request.heapOffset);
            assert(bound);
            (void)bound;

            resource.owned = request.virtualTexture;
            resource.texture = request.virtualTexture;
            resource.currentState = nvrhi::ResourceStates::Common;
        }
    }
    graph.m_transientStats.physicalTextureCount =
        static_cast<uint32_t>(physicalTextures.size() + heapRequests.size());

    std::vector<TransientAllocationRequest> bufferRequests;
    bufferRequests.reserve(graph.m_buffers.size());
    for (size_t i = 0; i < graph.m_buffers.size(); ++i)
    {
        if (i >= referencedBuffers.size() || !referencedBuffers[i])
            continue;
        if (graph.m_buffers[i].lifetime != GraphBuilder::ResourceLifetime::Transient)
            continue;
        if (i >= bufferLifetimes.size() || bufferLifetimes[i].lastPassOrder < 0)
            continue;

        const BufferDesc& desc = graph.m_buffers[i].desc;
        bufferRequests.push_back({
            i,
            bufferLifetimes[i].firstPassOrder,
            bufferLifetimes[i].lastPassOrder,
            hashBufferDesc(desc),
            hashBufferCompatibilityDesc(desc),
        });
    }
    graph.m_transientStats.transientBufferCount = static_cast<uint32_t>(bufferRequests.size());

    std::sort(bufferRequests.begin(), bufferRequests.end(), [](const TransientAllocationRequest& a, const TransientAllocationRequest& b) {
        if (a.firstPassOrder != b.firstPassOrder)
            return a.firstPassOrder < b.firstPassOrder;
        return a.resourceIndex < b.resourceIndex;
    });

    std::vector<PhysicalBufferSlot> physicalBuffers;
    std::vector<BufferHeapRequest> bufferHeapRequests;
    for (const TransientAllocationRequest& request : bufferRequests)
    {
        GraphBuilder::GraphBuffer& resource = graph.m_buffers[request.resourceIndex];

        const int physicalIndex = findAliasedBufferSlot(
            physicalBuffers,
            request.compatibilityHash,
            resource.desc.byteSize,
            request.firstPassOrder);

        if (physicalIndex >= 0)
        {
            PhysicalBufferSlot& slot = physicalBuffers[static_cast<size_t>(physicalIndex)];
            slot.lastPassOrder = std::max(slot.lastPassOrder, request.lastPassOrder);
            resource.owned = slot.handle;
            resource.buffer = slot.handle;
            resource.currentState = nvrhi::ResourceStates::Common;
            ++graph.m_transientStats.aliasedBufferCount;
            continue;
        }

        if (graph.m_renderBufferPool)
        {
            if (nvrhi::BufferHandle pooled = graph.m_renderBufferPool->tryAcquireBuffer(resource.desc))
            {
                PhysicalBufferSlot slot{};
                slot.compatibilityHash = request.compatibilityHash;
                slot.handle = pooled;
                slot.byteSize = std::max(resource.desc.byteSize, pooled->getDesc().byteSize);
                slot.lastPassOrder = request.lastPassOrder;
                physicalBuffers.push_back(slot);

                resource.owned = slot.handle;
                resource.buffer = resource.owned;
                resource.currentState = nvrhi::ResourceStates::Common;
                ++graph.m_transientStats.pooledBufferCount;
                continue;
            }
        }

        if (!canUseVirtualResources)
        {
            PhysicalBufferSlot slot{};
            slot.compatibilityHash = request.compatibilityHash;
            slot.handle = graph.m_renderBufferPool
                ? graph.m_renderBufferPool->acquireBuffer(resource.desc)
                : graph.createNativeBuffer(resource.desc);
            slot.byteSize = resource.desc.byteSize;
            if (slot.handle)
                slot.byteSize = std::max(slot.byteSize, slot.handle->getDesc().byteSize);
            slot.lastPassOrder = request.lastPassOrder;
            physicalBuffers.push_back(slot);

            resource.owned = slot.handle;
            assert(resource.owned);
            resource.buffer = resource.owned;
            resource.currentState = nvrhi::ResourceStates::Common;
            if (graph.m_renderBufferPool)
                ++graph.m_transientStats.pooledBufferCount;
            continue;
        }

        nvrhi::BufferHandle virtualBuffer = graph.createNativeBuffer(resource.desc, true);
        assert(virtualBuffer);
        const nvrhi::MemoryRequirements memReq = graph.m_device->getBufferMemoryRequirements(virtualBuffer);
        bufferHeapRequests.push_back({
            request.resourceIndex,
            request.firstPassOrder,
            request.lastPassOrder,
            memReq,
            virtualBuffer,
        });
    }

    if (!bufferHeapRequests.empty())
    {
        std::vector<HeapMemorySlot> memorySlots;
        uint64_t heapSize = 0;

        for (BufferHeapRequest& request : bufferHeapRequests)
        {
            const int slotIndex = findHeapMemorySlot(memorySlots, request.memReq, request.firstPassOrder);
            if (slotIndex >= 0)
            {
                HeapMemorySlot& slot = memorySlots[static_cast<size_t>(slotIndex)];
                if (slot.resourceIndex != SIZE_MAX)
                {
                    graph.m_bufferAliasingBarriers.push_back({
                        BufferHandle{ static_cast<uint32_t>(slot.resourceIndex) },
                        BufferHandle{ static_cast<uint32_t>(request.resourceIndex) },
                    });
                    ++graph.m_transientStats.aliasedBufferCount;
                }
                slot.lastPassOrder = std::max(slot.lastPassOrder, request.lastPassOrder);
                slot.resourceIndex = request.resourceIndex;
                request.heapOffset = slot.offset;
                continue;
            }

            const uint64_t alignment = std::max<uint64_t>(request.memReq.alignment, 1);
            const uint64_t alignedOffset = alignUp(heapSize, alignment);
            heapSize = alignedOffset + request.memReq.size;
            memorySlots.push_back({ alignedOffset, request.memReq.size, alignment, request.resourceIndex, request.lastPassOrder });
            request.heapOffset = alignedOffset;
        }

        nvrhi::HeapHandle heap = acquireHeap(heapSize, "rg_transient_buffer_heap");
        assert(heap);
        graph.m_transientHeaps.push_back(heap);
        graph.m_transientStats.bufferHeapBytes += heap->getDesc().capacity;
        graph.m_transientStats.placedBufferCount += static_cast<uint32_t>(bufferHeapRequests.size());

        for (const BufferHeapRequest& request : bufferHeapRequests)
        {
            GraphBuilder::GraphBuffer& resource = graph.m_buffers[request.resourceIndex];
            const bool bound = graph.m_device->bindBufferMemory(request.virtualBuffer, heap, request.heapOffset);
            assert(bound);
            (void)bound;

            resource.owned = request.virtualBuffer;
            resource.buffer = request.virtualBuffer;
            resource.currentState = nvrhi::ResourceStates::Common;
        }
    }
    graph.m_transientStats.physicalBufferCount =
        static_cast<uint32_t>(physicalBuffers.size() + bufferHeapRequests.size());
    updateHeapPoolStats();
}

} // namespace caustica::rg

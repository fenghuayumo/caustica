#pragma once

#include <render/graph/GraphBuilder.h>
#include <rhi/rhi.h>

#include <cstdint>
#include <vector>

namespace caustica::rg
{

// Aliasing barriers only sync the acquiring queue. Two transients may share
// memory only when every wave that touches them is on the same single queue.
[[nodiscard]] inline bool isSingleQueueMask(uint8_t queueMask)
{
    return queueMask != 0 && (queueMask & uint8_t(queueMask - 1)) == 0;
}

[[nodiscard]] inline bool canAliasTransientQueues(uint8_t requestMask, uint8_t slotMask)
{
    return isSingleQueueMask(requestMask) && requestMask == slotMask;
}

[[nodiscard]] inline uint8_t commandQueueBit(caustica::rhi::CommandQueue queue)
{
    return static_cast<uint8_t>(1u << uint32_t(queue));
}

class TransientResourceAllocator
{
public:
    void allocate(
        GraphBuilder& graph,
        const std::vector<bool>& referencedTextures,
        const std::vector<bool>& referencedBuffers,
        const std::vector<GraphBuilder::TransientLifetime>& textureLifetimes,
        const std::vector<GraphBuilder::TransientLifetime>& bufferLifetimes);
};

} // namespace caustica::rg

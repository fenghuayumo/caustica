#include <render/graph/RenderBufferPool.h>

#include <rhi/caustica/format.h>

#include <cassert>

namespace caustica::rg
{

int RenderBufferPool::findFreeSlot(const BufferDesc& desc) const
{
    const uint64_t compatibilityKey = hashBufferCompatibilityDesc(desc);
    int bestSlot = -1;
    uint64_t bestSize = UINT64_MAX;

    for (int i = 0; i < static_cast<int>(m_buffers.size()); ++i)
    {
        const PooledBuffer& entry = m_buffers[static_cast<size_t>(i)];
        if (entry.inUse)
            continue;
        if (hashBufferCompatibilityDesc(entry.desc) != compatibilityKey)
            continue;
        if (entry.desc.byteSize < desc.byteSize)
            continue;

        if (entry.desc.byteSize < bestSize)
        {
            bestSize = entry.desc.byteSize;
            bestSlot = i;
        }
    }
    return bestSlot;
}

nvrhi::BufferHandle RenderBufferPool::createPooledBuffer(const BufferDesc& desc)
{
    assert(m_device);

    nvrhi::BufferDesc nativeDesc;
    nativeDesc.debugName = desc.name.empty() ? "rg_buffer_pool" : desc.name.c_str();
    nativeDesc.byteSize = desc.byteSize;
    nativeDesc.structStride = desc.isStructuredBuffer ? desc.structuredStride : 0;
    nativeDesc.isConstantBuffer = desc.isConstantBuffer;
    nativeDesc.canHaveUAVs = desc.isUAV;
    nativeDesc.isVertexBuffer = desc.isVertexBuffer;
    nativeDesc.isIndexBuffer = desc.isIndexBuffer;
    nativeDesc.isDrawIndirectArgs = desc.isDrawIndirectArgs;
    nativeDesc.canHaveRawViews = desc.canHaveRawViews;
    nativeDesc.canHaveTypedViews = desc.canHaveTypedViews;
    nativeDesc.format = nvrhi::caustica::toNvrhiFormat(desc.format);
    nativeDesc.initialState = nvrhi::ResourceStates::Common;
    nativeDesc.keepInitialState = true;

    return m_device->createBuffer(nativeDesc);
}

nvrhi::BufferHandle RenderBufferPool::tryAcquireBuffer(const BufferDesc& desc)
{
    assert(m_device);

    if (const int slot = findFreeSlot(desc); slot >= 0)
    {
        PooledBuffer& entry = m_buffers[static_cast<size_t>(slot)];
        entry.inUse = true;
        entry.lastUsedFrame = m_frameIndex;
        return entry.handle;
    }

    return nullptr;
}

nvrhi::BufferHandle RenderBufferPool::acquireBuffer(const BufferDesc& desc)
{
    assert(m_device);

    if (nvrhi::BufferHandle existing = tryAcquireBuffer(desc))
        return existing;

    PooledBuffer entry{};
    entry.desc = desc;
    entry.handle = createPooledBuffer(desc);
    entry.inUse = true;
    entry.lastUsedFrame = m_frameIndex;
    m_buffers.push_back(entry);
    return entry.handle;
}

void RenderBufferPool::endFrame()
{
    for (PooledBuffer& entry : m_buffers)
        entry.inUse = false;
    ++m_frameIndex;
}

void RenderBufferPool::reset()
{
    m_buffers.clear();
    m_frameIndex = 0;
}

} // namespace caustica::rg

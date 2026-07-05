#pragma once

#include <render/graph/GpuTypes.h>
#include <rhi/nvrhi.h>

#include <cstdint>
#include <vector>

namespace caustica::rg
{

// Reuses transient graph buffers across frames (same model as RenderTargetPool).
class RenderBufferPool
{
public:
    void setDevice(nvrhi::IDevice* device) { m_device = device; }
    [[nodiscard]] nvrhi::IDevice* device() const { return m_device; }

    [[nodiscard]] nvrhi::BufferHandle acquireBuffer(const BufferDesc& desc);
    [[nodiscard]] nvrhi::BufferHandle tryAcquireBuffer(const BufferDesc& desc);
    void endFrame();

    void reset();

private:
    struct PooledBuffer
    {
        nvrhi::BufferHandle handle;
        BufferDesc desc;
        uint64_t lastUsedFrame = 0;
        bool inUse = false;
    };

    [[nodiscard]] int findFreeSlot(const BufferDesc& desc) const;
    [[nodiscard]] nvrhi::BufferHandle createPooledBuffer(const BufferDesc& desc);

    nvrhi::IDevice* m_device = nullptr;
    uint64_t m_frameIndex = 0;
    std::vector<PooledBuffer> m_buffers;
};

} // namespace caustica::rg

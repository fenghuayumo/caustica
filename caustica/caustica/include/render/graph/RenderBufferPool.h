#pragma once

#include <render/graph/GpuTypes.h>
#include <rhi/rhi.h>

#include <cstdint>
#include <vector>

namespace caustica::rg
{

// Reuses transient graph buffers across frames (same model as RenderTargetPool).
class RenderBufferPool
{
public:
    void setDevice(caustica::rhi::IDevice* device) { m_device = device; }
    [[nodiscard]] caustica::rhi::IDevice* device() const { return m_device; }

    [[nodiscard]] caustica::rhi::BufferHandle acquireBuffer(const BufferDesc& desc);
    [[nodiscard]] caustica::rhi::BufferHandle tryAcquireBuffer(const BufferDesc& desc);
    void endFrame();

    void reset();

private:
    struct PooledBuffer
    {
        caustica::rhi::BufferHandle handle;
        BufferDesc desc;
        uint64_t lastUsedFrame = 0;
        bool inUse = false;
    };

    [[nodiscard]] int findFreeSlot(const BufferDesc& desc) const;
    [[nodiscard]] caustica::rhi::BufferHandle createPooledBuffer(const BufferDesc& desc);

    caustica::rhi::IDevice* m_device = nullptr;
    uint64_t m_frameIndex = 0;
    std::vector<PooledBuffer> m_buffers;
};

} // namespace caustica::rg

#pragma once

#include <render/graph/GpuTypes.h>
#include <rhi/rhi.h>

#include <cstdint>
#include <vector>

namespace caustica::rg
{

// Reuses transient graph textures across frames; intra-frame aliasing is handled by GraphBuilder.
class RenderTargetPool
{
public:
    void setDevice(caustica::rhi::Device* device) { m_device = device; }
    [[nodiscard]] caustica::rhi::Device* device() const { return m_device; }

    [[nodiscard]] caustica::rhi::TextureHandle acquireTexture(const TextureDesc& desc);
    [[nodiscard]] caustica::rhi::TextureHandle tryAcquireTexture(const TextureDesc& desc);
    void endFrame();

    void reset();

private:
    struct PooledTexture
    {
        caustica::rhi::TextureHandle handle;
        TextureDesc desc;
        uint64_t lastUsedFrame = 0;
        bool inUse = false;
    };

    [[nodiscard]] int findFreeSlot(const TextureDesc& desc) const;
    [[nodiscard]] caustica::rhi::TextureHandle createPooledTexture(const TextureDesc& desc);

    caustica::rhi::Device* m_device = nullptr;
    uint64_t m_frameIndex = 0;
    std::vector<PooledTexture> m_textures;
};

} // namespace caustica::rg

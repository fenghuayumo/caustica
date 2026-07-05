#pragma once

#include <render/graph/GpuTypes.h>
#include <rhi/nvrhi.h>

#include <cstdint>
#include <vector>

namespace caustica::rg
{

// Reuses transient graph textures across frames (UE FRenderTargetPool-style).
class RenderTargetPool
{
public:
    void setDevice(nvrhi::IDevice* device) { m_device = device; }
    [[nodiscard]] nvrhi::IDevice* device() const { return m_device; }

    [[nodiscard]] nvrhi::TextureHandle acquireTexture(const TextureDesc& desc);
    void endFrame();

    void reset();

private:
    struct PooledTexture
    {
        nvrhi::TextureHandle handle;
        TextureDesc desc;
        uint64_t lastUsedFrame = 0;
        bool inUse = false;
    };

    [[nodiscard]] static uint64_t hashDesc(const TextureDesc& desc);
    [[nodiscard]] int findFreeSlot(const TextureDesc& desc) const;
    [[nodiscard]] nvrhi::TextureHandle createPooledTexture(const TextureDesc& desc);

    nvrhi::IDevice* m_device = nullptr;
    uint64_t m_frameIndex = 0;
    std::vector<PooledTexture> m_textures;
};

} // namespace caustica::rg

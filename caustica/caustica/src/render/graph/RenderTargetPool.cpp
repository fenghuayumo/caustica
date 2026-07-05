#include <render/graph/RenderTargetPool.h>

#include <rhi/caustica/format.h>

#include <cassert>

namespace caustica::rg
{

namespace
{
    uint64_t hashCombine(uint64_t seed, uint64_t value)
    {
        return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    }
}

uint64_t RenderTargetPool::hashDesc(const TextureDesc& desc)
{
    uint64_t hash = 0;
    hash = hashCombine(hash, desc.width);
    hash = hashCombine(hash, desc.height);
    hash = hashCombine(hash, desc.depth);
    hash = hashCombine(hash, desc.mipLevels);
    hash = hashCombine(hash, desc.arraySize);
    hash = hashCombine(hash, static_cast<uint64_t>(desc.format));
    hash = hashCombine(hash, desc.isRenderTarget ? 1u : 0u);
    hash = hashCombine(hash, desc.isUAV ? 1u : 0u);
    hash = hashCombine(hash, desc.isTypeless ? 1u : 0u);
    return hash;
}

int RenderTargetPool::findFreeSlot(const TextureDesc& desc) const
{
    const uint64_t key = hashDesc(desc);
    for (int i = 0; i < static_cast<int>(m_textures.size()); ++i)
    {
        const PooledTexture& entry = m_textures[static_cast<size_t>(i)];
        if (entry.inUse)
            continue;
        if (hashDesc(entry.desc) != key)
            continue;
        return i;
    }
    return -1;
}

nvrhi::TextureHandle RenderTargetPool::createPooledTexture(const TextureDesc& desc)
{
    assert(m_device);

    const FormatInfo formatInfo = getFormatInfo(desc.format);
    nvrhi::TextureDesc nativeDesc;
    nativeDesc.debugName = desc.name.empty() ? "rg_pool" : desc.name.c_str();
    nativeDesc.width = desc.width;
    nativeDesc.height = desc.height;
    nativeDesc.depth = desc.depth;
    nativeDesc.mipLevels = desc.mipLevels;
    nativeDesc.arraySize = desc.arraySize;
    nativeDesc.format = nvrhi::caustica::toNvrhiFormat(desc.format);
    nativeDesc.isRenderTarget = desc.isRenderTarget || formatInfo.isRenderTargetCompatible;
    nativeDesc.isUAV = desc.isUAV || formatInfo.isUAVCompatible;
    nativeDesc.isTypeless = desc.isTypeless;
    nativeDesc.initialState = nvrhi::ResourceStates::Common;
    nativeDesc.keepInitialState = true;

    return m_device->createTexture(nativeDesc);
}

nvrhi::TextureHandle RenderTargetPool::acquireTexture(const TextureDesc& desc)
{
    assert(m_device);

    if (const int slot = findFreeSlot(desc); slot >= 0)
    {
        PooledTexture& entry = m_textures[static_cast<size_t>(slot)];
        entry.inUse = true;
        entry.lastUsedFrame = m_frameIndex;
        return entry.handle;
    }

    PooledTexture entry{};
    entry.desc = desc;
    entry.handle = createPooledTexture(desc);
    entry.inUse = true;
    entry.lastUsedFrame = m_frameIndex;
    m_textures.push_back(entry);
    return entry.handle;
}

void RenderTargetPool::endFrame()
{
    for (PooledTexture& entry : m_textures)
        entry.inUse = false;
    ++m_frameIndex;
}

void RenderTargetPool::reset()
{
    m_textures.clear();
    m_frameIndex = 0;
}

} // namespace caustica::rg

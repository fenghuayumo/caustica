#include <render/graph/RenderTargetPool.h>

#include <rhi/caustica/format.h>

#include <cassert>

namespace caustica::rg
{

int RenderTargetPool::findFreeSlot(const TextureDesc& desc) const
{
    const uint64_t exactKey = hashTextureDesc(desc);
    for (int i = 0; i < static_cast<int>(m_textures.size()); ++i)
    {
        const PooledTexture& entry = m_textures[static_cast<size_t>(i)];
        if (entry.inUse)
            continue;
        if (hashTextureDesc(entry.desc) == exactKey)
            return i;
    }

    int bestSlot = -1;
    uint64_t bestArea = UINT64_MAX;
    const uint64_t requestArea = static_cast<uint64_t>(desc.width) * desc.height * desc.depth;

    for (int i = 0; i < static_cast<int>(m_textures.size()); ++i)
    {
        const PooledTexture& entry = m_textures[static_cast<size_t>(i)];
        if (entry.inUse)
            continue;
        if (!textureDescCovers(entry.desc, desc))
            continue;

        const uint64_t slotArea = static_cast<uint64_t>(entry.desc.width) * entry.desc.height * entry.desc.depth;
        if (slotArea < bestArea)
        {
            bestArea = slotArea;
            bestSlot = i;
        }
    }
    return bestSlot;
}

caustica::rhi::TextureHandle RenderTargetPool::createPooledTexture(const TextureDesc& desc)
{
    assert(m_device);

    const FormatInfo formatInfo = getFormatInfo(desc.format);
    caustica::rhi::TextureDesc nativeDesc;
    nativeDesc.debugName = desc.name.empty() ? "rg_pool" : desc.name.c_str();
    nativeDesc.width = desc.width;
    nativeDesc.height = desc.height;
    nativeDesc.depth = desc.depth;
    nativeDesc.mipLevels = desc.mipLevels;
    nativeDesc.arraySize = desc.arraySize;
    nativeDesc.format = toNativeFormat(desc.format);
    nativeDesc.isRenderTarget = desc.isRenderTarget || formatInfo.isRenderTargetCompatible;
    nativeDesc.isUAV = desc.isUAV || formatInfo.isUAVCompatible;
    nativeDesc.isTypeless = desc.isTypeless;
    nativeDesc.initialState = caustica::rhi::ResourceStates::Common;
    nativeDesc.keepInitialState = true;

    return m_device->createTexture(nativeDesc);
}

caustica::rhi::TextureHandle RenderTargetPool::tryAcquireTexture(const TextureDesc& desc)
{
    assert(m_device);

    if (const int slot = findFreeSlot(desc); slot >= 0)
    {
        PooledTexture& entry = m_textures[static_cast<size_t>(slot)];
        entry.inUse = true;
        entry.lastUsedFrame = m_frameIndex;
        return entry.handle;
    }

    return nullptr;
}

caustica::rhi::TextureHandle RenderTargetPool::acquireTexture(const TextureDesc& desc)
{
    assert(m_device);

    if (caustica::rhi::TextureHandle existing = tryAcquireTexture(desc))
        return existing;

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

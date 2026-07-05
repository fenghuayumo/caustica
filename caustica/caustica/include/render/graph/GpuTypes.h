#pragma once

#include <rhi/caustica/format.h>

#include <cstdint>
#include <string>

namespace caustica::rg
{

using Format = nvrhi::caustica::PixelFormat;
using FormatInfo = nvrhi::caustica::PixelFormatInfo;

[[nodiscard]] inline FormatInfo getFormatInfo(Format format)
{
    return nvrhi::caustica::getPixelFormatInfo(format);
}

[[nodiscard]] inline nvrhi::Format toNvrhiFormat(Format format)
{
    return nvrhi::caustica::toNvrhiFormat(format);
}

[[nodiscard]] inline Format fromNvrhiFormat(nvrhi::Format nativeFormat)
{
    return nvrhi::caustica::fromNvrhiFormat(nativeFormat);
}

struct TextureDesc
{
    std::string name;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t mipLevels = 1;
    uint32_t arraySize = 1;
    Format format = Format::RGBA8_UNORM;
    bool isRenderTarget = false;
    bool isUAV = false;
    bool isTypeless = false;
};

struct BufferDesc
{
    std::string name;
    uint64_t byteSize = 0;
    bool isConstantBuffer = false;
    bool isStructuredBuffer = false;
    uint32_t structuredStride = 0;
    bool isUAV = false;
    bool isVertexBuffer = false;
    bool isIndexBuffer = false;
    bool isDrawIndirectArgs = false;
    bool canHaveRawViews = false;
    bool canHaveTypedViews = false;
    Format format = Format::Unknown;
};

struct TextureHandle
{
    uint32_t index = UINT32_MAX;

    [[nodiscard]] bool isValid() const { return index != UINT32_MAX; }
};

struct BufferHandle
{
    uint32_t index = UINT32_MAX;

    [[nodiscard]] bool isValid() const { return index != UINT32_MAX; }
};

struct TransientResourceStats
{
    uint32_t transientTextureCount = 0;
    uint32_t transientBufferCount = 0;
    uint32_t physicalTextureCount = 0;
    uint32_t physicalBufferCount = 0;
    uint32_t aliasedTextureCount = 0;
    uint32_t aliasedBufferCount = 0;
    uint32_t pooledTextureCount = 0;
    uint32_t pooledBufferCount = 0;
    uint32_t placedTextureCount = 0;
    uint32_t placedBufferCount = 0;
    uint32_t aliasingBarrierCount = 0;
    uint32_t createdHeapCount = 0;
    uint32_t reusedHeapCount = 0;
    uint32_t pooledHeapCount = 0;
    uint64_t textureHeapBytes = 0;
    uint64_t bufferHeapBytes = 0;
    uint64_t pooledHeapBytes = 0;

    [[nodiscard]] uint64_t totalHeapBytes() const { return textureHeapBytes + bufferHeapBytes; }
};

namespace detail
{
    inline uint64_t hashCombine(uint64_t seed, uint64_t value)
    {
        return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    }
}

[[nodiscard]] inline uint64_t hashTextureDesc(const TextureDesc& desc)
{
    uint64_t hash = 0;
    hash = detail::hashCombine(hash, desc.width);
    hash = detail::hashCombine(hash, desc.height);
    hash = detail::hashCombine(hash, desc.depth);
    hash = detail::hashCombine(hash, desc.mipLevels);
    hash = detail::hashCombine(hash, desc.arraySize);
    hash = detail::hashCombine(hash, static_cast<uint64_t>(desc.format));
    hash = detail::hashCombine(hash, desc.isRenderTarget ? 1u : 0u);
    hash = detail::hashCombine(hash, desc.isUAV ? 1u : 0u);
    hash = detail::hashCombine(hash, desc.isTypeless ? 1u : 0u);
    return hash;
}

[[nodiscard]] inline uint64_t hashBufferDesc(const BufferDesc& desc)
{
    uint64_t hash = 0;
    hash = detail::hashCombine(hash, desc.byteSize);
    hash = detail::hashCombine(hash, desc.structuredStride);
    hash = detail::hashCombine(hash, static_cast<uint64_t>(desc.format));
    hash = detail::hashCombine(hash, desc.isConstantBuffer ? 1u : 0u);
    hash = detail::hashCombine(hash, desc.isStructuredBuffer ? 1u : 0u);
    hash = detail::hashCombine(hash, desc.isUAV ? 1u : 0u);
    hash = detail::hashCombine(hash, desc.isVertexBuffer ? 1u : 0u);
    hash = detail::hashCombine(hash, desc.isIndexBuffer ? 1u : 0u);
    hash = detail::hashCombine(hash, desc.isDrawIndirectArgs ? 1u : 0u);
    hash = detail::hashCombine(hash, desc.canHaveRawViews ? 1u : 0u);
    hash = detail::hashCombine(hash, desc.canHaveTypedViews ? 1u : 0u);
    return hash;
}

[[nodiscard]] inline uint64_t hashBufferCompatibilityDesc(const BufferDesc& desc)
{
    BufferDesc compatibilityDesc = desc;
    compatibilityDesc.byteSize = 0;
    compatibilityDesc.name.clear();
    return hashBufferDesc(compatibilityDesc);
}

[[nodiscard]] inline uint64_t hashTextureCompatibilityDesc(const TextureDesc& desc)
{
    TextureDesc compatibilityDesc = desc;
    compatibilityDesc.width = 0;
    compatibilityDesc.height = 0;
    compatibilityDesc.depth = 0;
    compatibilityDesc.name.clear();
    return hashTextureDesc(compatibilityDesc);
}

[[nodiscard]] inline bool textureDescCovers(const TextureDesc& slot, const TextureDesc& request)
{
    if (hashTextureCompatibilityDesc(slot) != hashTextureCompatibilityDesc(request))
        return false;
    return slot.width >= request.width
        && slot.height >= request.height
        && slot.depth >= request.depth;
}

} // namespace caustica::rg

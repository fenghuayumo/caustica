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

// Deprecated aliases kept for existing call sites.
[[nodiscard]] inline uint32_t toNativeFormat(Format format)
{
    return static_cast<uint32_t>(nvrhi::caustica::toNvrhiFormat(format));
}

[[nodiscard]] inline Format fromNativeFormat(uint32_t nativeFormat)
{
    return nvrhi::caustica::fromNvrhiFormat(static_cast<nvrhi::Format>(nativeFormat));
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

} // namespace caustica::rg

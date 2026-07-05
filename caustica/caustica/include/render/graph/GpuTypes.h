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

} // namespace caustica::rg

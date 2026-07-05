#pragma once

#include <cstdint>
#include <string>

namespace caustica::rg
{

enum class Format : uint16_t
{
    Unknown = 0,

    R8_UNORM,
    RG8_UNORM,
    RGBA8_UNORM,
    RGBA8_UNORM_SRGB,
    BGRA8_UNORM,
    BGRA8_UNORM_SRGB,

    R16_FLOAT,
    RG16_FLOAT,
    RGBA16_FLOAT,
    R32_FLOAT,
    RG32_FLOAT,
    RGBA32_FLOAT,

    R11G11B10_FLOAT,
    R10G10B10A2_UNORM,

    D24S8,
    D32_FLOAT,
};

struct FormatInfo
{
    bool isDepth = false;
    bool isStencil = false;
    bool isSRGB = false;
    bool isUAVCompatible = false;
    bool isRenderTargetCompatible = false;
    uint8_t blockSize = 1;
};

[[nodiscard]] FormatInfo getFormatInfo(Format format);
[[nodiscard]] uint32_t toNativeFormat(Format format);
[[nodiscard]] Format fromNativeFormat(uint32_t nativeFormat);

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

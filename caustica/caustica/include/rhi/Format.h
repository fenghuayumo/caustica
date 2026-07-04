#pragma once

#include <cstdint>

namespace caustica::rhi
{

// Engine-facing pixel format. Maps to nvrhi::Format only inside the backend adapter.
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

} // namespace caustica::rhi

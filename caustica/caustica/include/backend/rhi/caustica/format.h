#pragma once

#include <rhi/rhi_types.h>

#include <cstdint>

namespace caustica::rhi
{

static constexpr uint32_t c_ForkVersion = 1;

// Render-graph pixel format subset. Maps to caustica::rhi::Format.
enum class PixelFormat : uint16_t
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

struct PixelFormatInfo
{
    bool isDepth = false;
    bool isStencil = false;
    bool isSRGB = false;
    bool isUAVCompatible = false;
    bool isRenderTargetCompatible = false;
    uint8_t blockSize = 1;
};

[[nodiscard]] PixelFormatInfo getPixelFormatInfo(PixelFormat format);
[[nodiscard]] Format toRhiFormat(PixelFormat format);
[[nodiscard]] PixelFormat fromRhiFormat(Format format);

} // namespace caustica::rhi

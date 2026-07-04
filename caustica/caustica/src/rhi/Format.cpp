#include <rhi/Format.h>

namespace caustica::rhi
{

FormatInfo getFormatInfo(Format format)
{
    FormatInfo info{};

    switch (format)
    {
    case Format::RGBA8_UNORM_SRGB:
    case Format::BGRA8_UNORM_SRGB:
        info.isSRGB = true;
        [[fallthrough]];
    case Format::R8_UNORM:
    case Format::RG8_UNORM:
    case Format::RGBA8_UNORM:
    case Format::BGRA8_UNORM:
        info.isRenderTargetCompatible = true;
        break;
    case Format::R16_FLOAT:
    case Format::RG16_FLOAT:
    case Format::RGBA16_FLOAT:
    case Format::R32_FLOAT:
    case Format::RG32_FLOAT:
    case Format::RGBA32_FLOAT:
    case Format::R11G11B10_FLOAT:
        info.isUAVCompatible = true;
        info.isRenderTargetCompatible = true;
        break;
    case Format::D24S8:
        info.isDepth = true;
        info.isStencil = true;
        break;
    case Format::D32_FLOAT:
        info.isDepth = true;
        break;
    default:
        break;
    }

    return info;
}

} // namespace caustica::rhi

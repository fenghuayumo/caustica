#include <rhi/caustica/format.h>

namespace nvrhi::caustica
{

PixelFormatInfo getPixelFormatInfo(PixelFormat format)
{
    PixelFormatInfo info{};

    switch (format)
    {
    case PixelFormat::RGBA8_UNORM_SRGB:
    case PixelFormat::BGRA8_UNORM_SRGB:
        info.isSRGB = true;
        [[fallthrough]];
    case PixelFormat::R8_UNORM:
    case PixelFormat::RG8_UNORM:
    case PixelFormat::RGBA8_UNORM:
    case PixelFormat::BGRA8_UNORM:
        info.isRenderTargetCompatible = true;
        break;
    case PixelFormat::R16_FLOAT:
    case PixelFormat::RG16_FLOAT:
    case PixelFormat::RGBA16_FLOAT:
    case PixelFormat::R32_FLOAT:
    case PixelFormat::RG32_FLOAT:
    case PixelFormat::RGBA32_FLOAT:
    case PixelFormat::R11G11B10_FLOAT:
        info.isUAVCompatible = true;
        info.isRenderTargetCompatible = true;
        break;
    case PixelFormat::D24S8:
        info.isDepth = true;
        info.isStencil = true;
        break;
    case PixelFormat::D32_FLOAT:
        info.isDepth = true;
        break;
    default:
        break;
    }

    return info;
}

Format toNvrhiFormat(PixelFormat format)
{
    switch (format)
    {
    case PixelFormat::R8_UNORM:            return Format::R8_UNORM;
    case PixelFormat::RG8_UNORM:           return Format::RG8_UNORM;
    case PixelFormat::RGBA8_UNORM:         return Format::RGBA8_UNORM;
    case PixelFormat::RGBA8_UNORM_SRGB:    return Format::SRGBA8_UNORM;
    case PixelFormat::BGRA8_UNORM:         return Format::BGRA8_UNORM;
    case PixelFormat::BGRA8_UNORM_SRGB:    return Format::SBGRA8_UNORM;
    case PixelFormat::R16_FLOAT:           return Format::R16_FLOAT;
    case PixelFormat::RG16_FLOAT:          return Format::RG16_FLOAT;
    case PixelFormat::RGBA16_FLOAT:        return Format::RGBA16_FLOAT;
    case PixelFormat::R32_FLOAT:           return Format::R32_FLOAT;
    case PixelFormat::RG32_FLOAT:          return Format::RG32_FLOAT;
    case PixelFormat::RGBA32_FLOAT:        return Format::RGBA32_FLOAT;
    case PixelFormat::R11G11B10_FLOAT:     return Format::R11G11B10_FLOAT;
    case PixelFormat::R10G10B10A2_UNORM:   return Format::R10G10B10A2_UNORM;
    case PixelFormat::D24S8:              return Format::D24S8;
    case PixelFormat::D32_FLOAT:           return Format::D32;
    default:                               return Format::UNKNOWN;
    }
}

PixelFormat fromNvrhiFormat(Format format)
{
    switch (format)
    {
    case Format::R8_UNORM:            return PixelFormat::R8_UNORM;
    case Format::RG8_UNORM:           return PixelFormat::RG8_UNORM;
    case Format::RGBA8_UNORM:         return PixelFormat::RGBA8_UNORM;
    case Format::SRGBA8_UNORM:        return PixelFormat::RGBA8_UNORM_SRGB;
    case Format::BGRA8_UNORM:         return PixelFormat::BGRA8_UNORM;
    case Format::SBGRA8_UNORM:        return PixelFormat::BGRA8_UNORM_SRGB;
    case Format::R16_FLOAT:           return PixelFormat::R16_FLOAT;
    case Format::RG16_FLOAT:          return PixelFormat::RG16_FLOAT;
    case Format::RGBA16_FLOAT:        return PixelFormat::RGBA16_FLOAT;
    case Format::R32_FLOAT:           return PixelFormat::R32_FLOAT;
    case Format::RG32_FLOAT:          return PixelFormat::RG32_FLOAT;
    case Format::RGBA32_FLOAT:        return PixelFormat::RGBA32_FLOAT;
    case Format::R11G11B10_FLOAT:     return PixelFormat::R11G11B10_FLOAT;
    case Format::R10G10B10A2_UNORM:   return PixelFormat::R10G10B10A2_UNORM;
    case Format::D24S8:              return PixelFormat::D24S8;
    case Format::D32:                return PixelFormat::D32_FLOAT;
    default:                          return PixelFormat::Unknown;
    }
}

} // namespace nvrhi::caustica

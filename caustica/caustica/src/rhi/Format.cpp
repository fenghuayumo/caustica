#include <rhi/Format.h>

#include <rhi/nvrhi.h>

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

uint32_t toNativeFormat(Format format)
{
    switch (format)
    {
    case Format::R8_UNORM:            return static_cast<uint32_t>(nvrhi::Format::R8_UNORM);
    case Format::RG8_UNORM:           return static_cast<uint32_t>(nvrhi::Format::RG8_UNORM);
    case Format::RGBA8_UNORM:         return static_cast<uint32_t>(nvrhi::Format::RGBA8_UNORM);
    case Format::RGBA8_UNORM_SRGB:    return static_cast<uint32_t>(nvrhi::Format::SRGBA8_UNORM);
    case Format::BGRA8_UNORM:         return static_cast<uint32_t>(nvrhi::Format::BGRA8_UNORM);
    case Format::BGRA8_UNORM_SRGB:    return static_cast<uint32_t>(nvrhi::Format::SBGRA8_UNORM);
    case Format::R16_FLOAT:           return static_cast<uint32_t>(nvrhi::Format::R16_FLOAT);
    case Format::RG16_FLOAT:          return static_cast<uint32_t>(nvrhi::Format::RG16_FLOAT);
    case Format::RGBA16_FLOAT:        return static_cast<uint32_t>(nvrhi::Format::RGBA16_FLOAT);
    case Format::R32_FLOAT:           return static_cast<uint32_t>(nvrhi::Format::R32_FLOAT);
    case Format::RG32_FLOAT:          return static_cast<uint32_t>(nvrhi::Format::RG32_FLOAT);
    case Format::RGBA32_FLOAT:        return static_cast<uint32_t>(nvrhi::Format::RGBA32_FLOAT);
    case Format::R11G11B10_FLOAT:     return static_cast<uint32_t>(nvrhi::Format::R11G11B10_FLOAT);
    case Format::R10G10B10A2_UNORM:   return static_cast<uint32_t>(nvrhi::Format::R10G10B10A2_UNORM);
    case Format::D24S8:              return static_cast<uint32_t>(nvrhi::Format::D24S8);
    case Format::D32_FLOAT:           return static_cast<uint32_t>(nvrhi::Format::D32);
    default:                          return static_cast<uint32_t>(nvrhi::Format::UNKNOWN);
    }
}

Format fromNativeFormat(uint32_t nativeFormat)
{
    switch (static_cast<nvrhi::Format>(nativeFormat))
    {
    case nvrhi::Format::R8_UNORM:            return Format::R8_UNORM;
    case nvrhi::Format::RG8_UNORM:           return Format::RG8_UNORM;
    case nvrhi::Format::RGBA8_UNORM:         return Format::RGBA8_UNORM;
    case nvrhi::Format::SRGBA8_UNORM:         return Format::RGBA8_UNORM_SRGB;
    case nvrhi::Format::BGRA8_UNORM:         return Format::BGRA8_UNORM;
    case nvrhi::Format::SBGRA8_UNORM:        return Format::BGRA8_UNORM_SRGB;
    case nvrhi::Format::R16_FLOAT:          return Format::R16_FLOAT;
    case nvrhi::Format::RG16_FLOAT:         return Format::RG16_FLOAT;
    case nvrhi::Format::RGBA16_FLOAT:       return Format::RGBA16_FLOAT;
    case nvrhi::Format::R32_FLOAT:          return Format::R32_FLOAT;
    case nvrhi::Format::RG32_FLOAT:         return Format::RG32_FLOAT;
    case nvrhi::Format::RGBA32_FLOAT:       return Format::RGBA32_FLOAT;
    case nvrhi::Format::R11G11B10_FLOAT:    return Format::R11G11B10_FLOAT;
    case nvrhi::Format::R10G10B10A2_UNORM:   return Format::R10G10B10A2_UNORM;
    case nvrhi::Format::D24S8:              return Format::D24S8;
    case nvrhi::Format::D32:                return Format::D32_FLOAT;
    default:                                return Format::Unknown;
    }
}

} // namespace caustica::rhi

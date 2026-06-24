#pragma once

#include <rhi/nvrhi.h>
#include <map>
#include <memory>

namespace caustica { struct LoadedTexture; }

// Texture compression type for batch compression with nvtt_export.
// Moved from editor/SampleCommon/SampleCommon.h
enum class TextureCompressionType
{
    Normalmap,
    GenericSRGB,
    GenericLinear,
};

// Estimated GPU memory size of a texture.
// Moved from editor/SampleCommon/SampleCommon.cpp
inline uint64_t GetEstimatedTextureSize(const nvrhi::TextureDesc& desc)
{
    nvrhi::FormatInfo fi = nvrhi::getFormatInfo(desc.format);
    uint64_t pixelsCount = 0;
    uint32_t w = desc.width, h = desc.height, d = desc.depth;
    for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
    {
        pixelsCount += size_t(w) * h * d * desc.arraySize;
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
        d = std::max(1u, d >> 1);
    }
    return pixelsCount / fi.blockSize * fi.bytesPerBlock;
}

// Batch-compress uncompressed textures using nvtt_export.
// Moved from editor/SampleCommon/SampleCommon.cpp
bool CompressTextures(std::map<std::shared_ptr<caustica::LoadedTexture>, TextureCompressionType>& uncompressedTextures);

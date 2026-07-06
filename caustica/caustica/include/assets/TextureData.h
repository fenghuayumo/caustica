#pragma once

#include <assets/LoadedTexture.h>
#include <rhi/nvrhi.h>

#include <memory>
#include <vector>

namespace caustica
{
class IBlob;

struct TextureSubresourceData
{
    size_t rowPitch = 0;
    size_t depthPitch = 0;
    ptrdiff_t dataOffset = 0;
    size_t dataSize = 0;
};

struct TextureData : public LoadedTexture
{
    std::shared_ptr<IBlob> data;

    nvrhi::Format format = nvrhi::Format::UNKNOWN;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t arraySize = 1;
    uint32_t mipLevels = 1;
    nvrhi::TextureDimension dimension = nvrhi::TextureDimension::Unknown;
    bool isRenderTarget = false;
    bool forceSRGB = false;

    // ArraySlice -> MipLevel -> TextureSubresourceData
    std::vector<std::vector<TextureSubresourceData>> dataLayout;
};

} // namespace caustica

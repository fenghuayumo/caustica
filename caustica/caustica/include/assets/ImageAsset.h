#pragma once

#include <assets/AssetId.h>
#include <core/DescriptorHandle.h>
#include <rhi/rhi.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{
class IBlob;

enum class TextureAlphaMode
{
    UNKNOWN = 0,
    STRAIGHT = 1,
    PREMULTIPLIED = 2,
    OPAQUE_ = 3,
    CUSTOM = 4,
};

struct GltfInlineData
{
    std::shared_ptr<caustica::IBlob> buffer;
    std::string name;
    std::string mimeType;
};

struct FilePathOrInlineData
{
    std::string path;
    std::shared_ptr<GltfInlineData> data;

    operator bool() const { return !path.empty() || data != nullptr; }

    bool operator==(FilePathOrInlineData const& other) const
    {
        return path == other.path && data == other.data;
    }

    bool operator!=(FilePathOrInlineData const& other) const { return !(*this == other); }

    std::string const& toString() const { return data ? data->name : path; }
};

struct TextureSwizzle
{
    FilePathOrInlineData source;
    int numChannels = 0;
    std::array<int, 4> channels;

    TextureSwizzle() { channels.fill(-1); }
};

struct TextureSubresourceData
{
    size_t rowPitch = 0;
    size_t depthPitch = 0;
    ptrdiff_t dataOffset = 0;
    size_t dataSize = 0;
};

struct GpuImage
{
    caustica::rhi::TextureHandle texture;
    DescriptorHandle bindlessDescriptor;
};

struct ImageAsset
{
    AssetId id = AssetId::invalid();
    std::string path;
    std::string mimeType;

    std::shared_ptr<IBlob> data;
    TextureAlphaMode alphaMode = TextureAlphaMode::UNKNOWN;
    uint32_t originalBitsPerPixel = 0;

    caustica::rhi::Format format = caustica::rhi::Format::UNKNOWN;
    uint32_t width = 1;
    uint32_t height = 1;
    uint32_t depth = 1;
    uint32_t arraySize = 1;
    uint32_t mipLevels = 1;
    caustica::rhi::TextureDimension dimension = caustica::rhi::TextureDimension::Unknown;
    bool isRenderTarget = false;
    bool forceSRGB = false;

    std::vector<std::vector<TextureSubresourceData>> dataLayout;
    std::vector<TextureSwizzle> swizzleOptions;
    GpuImage gpu;
};

} // namespace caustica

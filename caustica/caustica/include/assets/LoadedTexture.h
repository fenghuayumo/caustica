#pragma once

#include <core/DescriptorHandle.h>
#include <rhi/nvrhi.h>

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{
class IBlob;
}

namespace caustica
{

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

    std::string const& ToString() const { return data ? data->name : path; }
};

struct TextureSwizzle
{
    FilePathOrInlineData source;
    int numChannels = 0;
    std::array<int, 4> channels;

    TextureSwizzle() { channels.fill(-1); }
};

struct LoadedTexture
{
    nvrhi::TextureHandle texture;
    TextureAlphaMode alphaMode = TextureAlphaMode::UNKNOWN;
    uint32_t originalBitsPerPixel = 0;
    DescriptorHandle bindlessDescriptor;
    std::string path;
    std::string mimeType;
    uint64_t assetIdLow = 0;
    uint64_t assetIdHigh = 0;
    std::vector<TextureSwizzle> swizzleOptions;
};

} // namespace caustica

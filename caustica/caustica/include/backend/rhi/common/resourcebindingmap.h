#pragma once

#include <rhi/common/containers.h>
#include <rhi/rhi_types.h>
#include <unordered_map>

namespace caustica::rhi {

// describes a texture binding --- used to manage SRV / VkImageView per texture
struct TextureBindingKey : public TextureSubresourceSet
{
    Format format;
    bool isReadOnlyDSV;

    TextureBindingKey()
    {
    }

    TextureBindingKey(const TextureSubresourceSet& b, Format _format, bool _isReadOnlyDSV = false)
        : TextureSubresourceSet(b)
        , format(_format)
        , isReadOnlyDSV(_isReadOnlyDSV)
    {
    }

    bool operator== (const TextureBindingKey& other) const
    {
        return format == other.format &&
            static_cast<const TextureSubresourceSet&>(*this) == static_cast<const TextureSubresourceSet&>(other) &&
            isReadOnlyDSV == other.isReadOnlyDSV;
    }
};

template <typename T>
using TextureBindingKey_HashMap = std::unordered_map<TextureBindingKey, T>;

struct BufferBindingKey : public BufferRange
{
    Format format;
    ResourceType type;

    BufferBindingKey()
    { }

    BufferBindingKey(const BufferRange& range, Format _format, ResourceType _type)
        : BufferRange(range)
        , format(_format)
        , type(_type)
    { }

    bool operator== (const BufferBindingKey& other) const
    {
        return format == other.format &&
            type == other.type &&
            static_cast<const BufferRange&>(*this) == static_cast<const BufferRange&>(other);
    }
};

} // namespace caustica::rhi

namespace std
{
    template<> struct hash<caustica::rhi::TextureBindingKey>
    {
        std::size_t operator()(caustica::rhi::TextureBindingKey const& s) const noexcept
        {
            return std::hash<caustica::rhi::Format>()(s.format)
                ^ std::hash<caustica::rhi::TextureSubresourceSet>()(s)
                ^ std::hash<bool>()(s.isReadOnlyDSV);
        }
    };

    template<> struct hash<caustica::rhi::BufferBindingKey>
    {
        std::size_t operator()(caustica::rhi::BufferBindingKey const& s) const noexcept
        {
            return std::hash<caustica::rhi::Format>()(s.format)
                ^ std::hash<caustica::rhi::BufferRange>()(s);
        }
    };
}
#pragma once

#include <core/uuid.h>
#include <cstdint>
#include <functional>
#include <string>

namespace caustica
{

struct AssetId
{
    uint64_t low = 0;
    uint64_t high = 0;

    static AssetId generate();
    static AssetId invalid() { return {}; }

    bool isValid() const { return low != 0 || high != 0; }
    explicit operator bool() const { return isValid(); }

    bool operator==(AssetId other) const { return low == other.low && high == other.high; }
    bool operator!=(AssetId other) const { return !(*this == other); }
    bool operator<(AssetId other) const
    {
        return high != other.high ? high < other.high : low < other.low;
    }

    std::string toString() const;
    static AssetId fromString(const std::string& str);

    struct Hash
    {
        size_t operator()(AssetId id) const
        {
            return std::hash<uint64_t>()(id.low) ^ (std::hash<uint64_t>()(id.high) << 1);
        }
    };
};

enum class AssetType : uint8_t
{
    Unknown = 0,
    Texture,
    Mesh,
    Material,
    Shader,
    Scene,
    Animation,
    Audio,
    Font,
    Skeleton,
    PointCloud,
    Gaussian,
    Count
};

const char* assetTypeToString(AssetType type);

enum class AssetState : uint8_t
{
    Unknown,
    Loading,
    Loaded,
    Failed,
    Unloaded,
    UploadedGpu,
};

struct AssetMetadata
{
    AssetId    id;
    AssetType  type = AssetType::Unknown;
    AssetState state = AssetState::Unknown;
    uint32_t   version = 0;
    std::string path;
    std::string sourceFile;
};

} // namespace caustica

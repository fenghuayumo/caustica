#pragma once

#include <core/uuid.h>
#include <cstdint>
#include <functional>
#include <string>

// =============================================================================
// AssetId — universally unique identifier for an asset.
//
// Wraps a UUID for stable identification across sessions and machines.
// =============================================================================

namespace caustica
{

struct AssetId
{
    uint64_t low = 0;
    uint64_t high = 0;

    static AssetId Generate();
    static AssetId Invalid() { return {}; }

    bool IsValid() const { return low != 0 || high != 0; }
    explicit operator bool() const { return IsValid(); }

    bool operator==(AssetId other) const { return low == other.low && high == other.high; }
    bool operator!=(AssetId other) const { return !(*this == other); }
    bool operator<(AssetId other) const
    {
        return high != other.high ? high < other.high : low < other.low;
    }

    std::string ToString() const;
    static AssetId FromString(const std::string& str);

    struct Hash
    {
        size_t operator()(AssetId id) const
        {
            return std::hash<uint64_t>()(id.low) ^ (std::hash<uint64_t>()(id.high) << 1);
        }
    };
};

// =============================================================================
// AssetType — categories of assets managed by the pipeline.
// =============================================================================
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

const char* AssetTypeToString(AssetType type);

// =============================================================================
// AssetState — lifecycle state of an asset in the registry.
// =============================================================================
enum class AssetState : uint8_t
{
    Unknown,
    Loading,
    Loaded,
    Failed,
    Unloaded,
    UploadedGpu,
};

// =============================================================================
// AssetMetadata — registry entry for a single asset.
// =============================================================================
struct AssetMetadata
{
    AssetId    id;
    AssetType  type = AssetType::Unknown;
    AssetState state = AssetState::Unknown;
    uint32_t   version = 0;        // bumped on each reload
    std::string path;              // canonical filesystem path
    std::string sourceFile;        // original source (may differ from path)
    uint64_t   fileSize = 0;
    uint64_t   lastModified = 0;   // file write time for change detection
};

} // namespace caustica

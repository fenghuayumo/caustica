#include <assets/AssetId.h>
#include <core/uuid.h>

#include <sstream>
#include <iomanip>

namespace caustica
{

AssetId AssetId::generate()
{
    AssetId id;
    id.low = GenerateRandom64();
    id.high = GenerateRandom64();
    return id;
}

std::string AssetId::toString() const
{
    std::ostringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(16) << high
       << std::setw(16) << low;
    return ss.str();
}

AssetId AssetId::fromString(const std::string& str)
{
    if (str.size() < 32)
        return invalid();
    AssetId id;
    id.high = std::stoull(str.substr(0, 16), nullptr, 16);
    id.low = std::stoull(str.substr(16, 16), nullptr, 16);
    return id;
}

const char* assetTypeToString(AssetType type)
{
    switch (type)
    {
    case AssetType::Unknown:    return "Unknown";
    case AssetType::Texture:    return "Texture";
    case AssetType::Mesh:       return "Mesh";
    case AssetType::Material:   return "Material";
    case AssetType::Shader:     return "Shader";
    case AssetType::Scene:      return "Scene";
    case AssetType::Animation:  return "Animation";
    case AssetType::Audio:      return "Audio";
    case AssetType::Font:       return "Font";
    case AssetType::Skeleton:   return "Skeleton";
    case AssetType::PointCloud: return "PointCloud";
    case AssetType::Gaussian:   return "Gaussian";
    default:                    return "Unknown";
    }
}

} // namespace caustica

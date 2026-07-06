#pragma once

#include <assets/AssetId.h>

#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace caustica
{

class AssetRegistry
{
public:
    AssetId registerAsset(const std::filesystem::path& path, AssetType type);
    void unregisterAsset(const AssetId& id);

    [[nodiscard]] AssetId findByPath(const std::filesystem::path& path) const;
    void setState(const AssetId& id, AssetState state);

private:
    mutable std::shared_mutex m_Mutex;
    std::unordered_map<std::string, AssetId> m_PathToId;
    std::unordered_map<AssetId, std::shared_ptr<AssetMetadata>, AssetId::Hash> m_Metadata;
};

} // namespace caustica

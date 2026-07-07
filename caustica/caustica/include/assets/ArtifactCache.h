#pragma once

#include <assets/AssetId.h>

#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace caustica
{

struct ArtifactRecord
{
    AssetId sourceAsset = AssetId::invalid();
    std::string kind;
    std::filesystem::path path;
    std::string key;
};

class ArtifactCache
{
public:
    void put(AssetId sourceAsset, std::string kind, std::string key, std::filesystem::path path);
    [[nodiscard]] std::optional<ArtifactRecord> find(AssetId sourceAsset, const std::string& kind, const std::string& key) const;
    void removeAsset(AssetId sourceAsset);
    void clear();

private:
    static std::string makeCacheKey(AssetId sourceAsset, const std::string& kind, const std::string& key);

    mutable std::shared_mutex m_Mutex;
    std::unordered_map<std::string, ArtifactRecord> m_Records;
};

} // namespace caustica

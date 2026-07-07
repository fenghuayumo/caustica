#include <assets/ArtifactCache.h>

#include <mutex>

namespace caustica
{

void ArtifactCache::put(AssetId sourceAsset, std::string kind, std::string key, std::filesystem::path path)
{
    if (!sourceAsset || kind.empty() || key.empty() || path.empty())
        return;

    ArtifactRecord record;
    record.sourceAsset = sourceAsset;
    record.kind = std::move(kind);
    record.key = std::move(key);
    record.path = std::move(path);

    std::unique_lock lock(m_Mutex);
    m_Records[makeCacheKey(record.sourceAsset, record.kind, record.key)] = std::move(record);
}

std::optional<ArtifactRecord> ArtifactCache::find(
    AssetId sourceAsset,
    const std::string& kind,
    const std::string& key) const
{
    std::shared_lock lock(m_Mutex);
    if (auto it = m_Records.find(makeCacheKey(sourceAsset, kind, key)); it != m_Records.end())
        return it->second;
    return std::nullopt;
}

void ArtifactCache::removeAsset(AssetId sourceAsset)
{
    std::unique_lock lock(m_Mutex);
    for (auto it = m_Records.begin(); it != m_Records.end();)
    {
        if (it->second.sourceAsset == sourceAsset)
            it = m_Records.erase(it);
        else
            ++it;
    }
}

void ArtifactCache::clear()
{
    std::unique_lock lock(m_Mutex);
    m_Records.clear();
}

std::string ArtifactCache::makeCacheKey(AssetId sourceAsset, const std::string& kind, const std::string& key)
{
    return sourceAsset.toString() + "|" + kind + "|" + key;
}

} // namespace caustica

#include <assets/AssetRegistry.h>
#include <core/PathUtils.h>
#include <core/log.h>

namespace caustica
{

AssetId AssetRegistry::registerAsset(const std::filesystem::path& path, AssetType type)
{
    // One identity policy for the whole engine: absolute + lexically normal +
    // generic separators + case-folded on Windows (see canonicalAssetKey).
    std::string canonical = canonicalAssetKey(path);

    std::unique_lock lock(m_mutex);

    if (auto pathIt = m_pathToId.find(canonical); pathIt != m_pathToId.end())
    {
        if (auto metaIt = m_metadata.find(pathIt->second); metaIt != m_metadata.end())
        {
            metaIt->second->state = AssetState::Unknown;
            return pathIt->second;
        }
    }

    AssetId id = AssetId::generate();

    auto meta = std::make_shared<AssetMetadata>();
    meta->id = id;
    meta->type = type;
    meta->state = AssetState::Unknown;
    meta->path = canonical;
    meta->sourceFile = path.string();

    m_pathToId[canonical] = id;
    m_metadata[id] = meta;

    caustica::debug("AssetRegistry: registered %s as %s [%s]",
        canonical.c_str(), assetTypeToString(type), id.toString().c_str());

    return id;
}

void AssetRegistry::unregisterAsset(const AssetId& id)
{
    std::unique_lock lock(m_mutex);

    auto metaIt = m_metadata.find(id);
    if (metaIt == m_metadata.end())
        return;

    m_pathToId.erase(metaIt->second->path);
    m_metadata.erase(metaIt);
}

AssetId AssetRegistry::findByPath(const std::filesystem::path& path) const
{
    std::string canonical = canonicalAssetKey(path);

    std::shared_lock lock(m_mutex);
    auto it = m_pathToId.find(canonical);
    return it != m_pathToId.end() ? it->second : AssetId::invalid();
}

void AssetRegistry::setState(const AssetId& id, AssetState state)
{
    std::unique_lock lock(m_mutex);
    if (auto it = m_metadata.find(id); it != m_metadata.end())
        it->second->state = state;
}

} // namespace caustica

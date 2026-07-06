#include <assets/AssetRegistry.h>
#include <core/log.h>

namespace caustica
{

AssetId AssetRegistry::registerAsset(const std::filesystem::path& path, AssetType type)
{
    std::string canonical = std::filesystem::absolute(path).string();

    std::unique_lock lock(m_Mutex);

    if (auto pathIt = m_PathToId.find(canonical); pathIt != m_PathToId.end())
    {
        if (auto metaIt = m_Metadata.find(pathIt->second); metaIt != m_Metadata.end())
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

    m_PathToId[canonical] = id;
    m_Metadata[id] = meta;

    caustica::debug("AssetRegistry: registered %s as %s [%s]",
        canonical.c_str(), assetTypeToString(type), id.toString().c_str());

    return id;
}

void AssetRegistry::unregisterAsset(const AssetId& id)
{
    std::unique_lock lock(m_Mutex);

    auto metaIt = m_Metadata.find(id);
    if (metaIt == m_Metadata.end())
        return;

    m_PathToId.erase(metaIt->second->path);
    m_Metadata.erase(metaIt);
}

AssetId AssetRegistry::findByPath(const std::filesystem::path& path) const
{
    std::string canonical = std::filesystem::absolute(path).string();

    std::shared_lock lock(m_Mutex);
    auto it = m_PathToId.find(canonical);
    return it != m_PathToId.end() ? it->second : AssetId::invalid();
}

void AssetRegistry::setState(const AssetId& id, AssetState state)
{
    std::unique_lock lock(m_Mutex);
    if (auto it = m_Metadata.find(id); it != m_Metadata.end())
        it->second->state = state;
}

} // namespace caustica

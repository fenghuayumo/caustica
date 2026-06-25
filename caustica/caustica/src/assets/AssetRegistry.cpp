#include <assets/AssetRegistry.h>
#include <core/log.h>

#include <algorithm>

namespace caustica
{

// =============================================================================
// Register
// =============================================================================
AssetId AssetRegistry::Register(const std::filesystem::path& path, AssetType type)
{
    std::string canonical = std::filesystem::absolute(path).string();

    std::unique_lock lock(m_Mutex);

    // Check if already registered
    auto pathIt = m_PathToId.find(canonical);
    if (pathIt != m_PathToId.end())
    {
        auto metaIt = m_Metadata.find(pathIt->second);
        if (metaIt != m_Metadata.end())
        {
            metaIt->second->state = AssetState::Unknown;
            return pathIt->second;
        }
    }

    AssetId id = AssetId::Generate();

    auto meta = std::make_shared<AssetMetadata>();
    meta->id = id;
    meta->type = type;
    meta->state = AssetState::Unknown;
    meta->version = 0;
    meta->path = canonical;
    meta->sourceFile = path.string();

    m_PathToId[canonical] = id;
    m_Metadata[id] = meta;
    m_Dependencies[id] = {};
    m_Dependents[id] = {};

    caustica::debug("AssetRegistry: registered %s as %s [%s]",
                    canonical.c_str(), AssetTypeToString(type), id.ToString().c_str());

    return id;
}

// =============================================================================
// Unregister
// =============================================================================
void AssetRegistry::Unregister(const AssetId& id)
{
    std::unique_lock lock(m_Mutex);

    auto metaIt = m_Metadata.find(id);
    if (metaIt == m_Metadata.end())
        return;

    // Remove path mapping
    m_PathToId.erase(metaIt->second->path);

    // Remove dependency edges
    auto depIt = m_Dependencies.find(id);
    if (depIt != m_Dependencies.end())
    {
        for (const AssetId& dep : depIt->second)
        {
            auto depOfIt = m_Dependents.find(dep);
            if (depOfIt != m_Dependents.end())
                depOfIt->second.erase(id);
        }
        m_Dependencies.erase(depIt);
    }

    auto depOfIt = m_Dependents.find(id);
    if (depOfIt != m_Dependents.end())
    {
        for (const AssetId& depOf : depOfIt->second)
        {
            auto depIt2 = m_Dependencies.find(depOf);
            if (depIt2 != m_Dependencies.end())
                depIt2->second.erase(id);
        }
        m_Dependents.erase(depOfIt);
    }

    m_Metadata.erase(metaIt);
    m_ChangeCallbacks.erase(id);
}

// =============================================================================
// FindByPath
// =============================================================================
AssetId AssetRegistry::FindByPath(const std::filesystem::path& path) const
{
    std::string canonical = std::filesystem::absolute(path).string();

    std::shared_lock lock(m_Mutex);
    auto it = m_PathToId.find(canonical);
    return it != m_PathToId.end() ? it->second : AssetId::Invalid();
}

std::filesystem::path AssetRegistry::GetPath(const AssetId& id) const
{
    std::shared_lock lock(m_Mutex);
    auto it = m_Metadata.find(id);
    return it != m_Metadata.end() ? std::filesystem::path(it->second->path) : std::filesystem::path();
}

std::shared_ptr<AssetMetadata> AssetRegistry::GetMetadata(const AssetId& id) const
{
    std::shared_lock lock(m_Mutex);
    auto it = m_Metadata.find(id);
    return it != m_Metadata.end() ? it->second : nullptr;
}

AssetType AssetRegistry::GetType(const AssetId& id) const
{
    auto meta = GetMetadata(id);
    return meta ? meta->type : AssetType::Unknown;
}

AssetState AssetRegistry::GetState(const AssetId& id) const
{
    auto meta = GetMetadata(id);
    return meta ? meta->state : AssetState::Unknown;
}

uint32_t AssetRegistry::GetVersion(const AssetId& id) const
{
    auto meta = GetMetadata(id);
    return meta ? meta->version : 0;
}

// =============================================================================
// State Management
// =============================================================================
void AssetRegistry::SetState(const AssetId& id, AssetState state)
{
    std::unique_lock lock(m_Mutex);
    auto it = m_Metadata.find(id);
    if (it != m_Metadata.end())
        it->second->state = state;
}

void AssetRegistry::IncrementVersion(const AssetId& id)
{
    std::unique_lock lock(m_Mutex);
    auto it = m_Metadata.find(id);
    if (it != m_Metadata.end())
        it->second->version++;
}

// =============================================================================
// Dependency Tracking
// =============================================================================
void AssetRegistry::AddDependency(const AssetId& asset, const AssetId& dependsOn)
{
    if (asset == dependsOn || HasCircularDependency(asset, dependsOn))
        return;

    std::unique_lock lock(m_Mutex);
    m_Dependencies[asset].insert(dependsOn);
    m_Dependents[dependsOn].insert(asset);
}

void AssetRegistry::RemoveDependency(const AssetId& asset, const AssetId& dependsOn)
{
    std::unique_lock lock(m_Mutex);
    auto it = m_Dependencies.find(asset);
    if (it != m_Dependencies.end())
        it->second.erase(dependsOn);
    auto it2 = m_Dependents.find(dependsOn);
    if (it2 != m_Dependents.end())
        it2->second.erase(asset);
}

std::vector<AssetId> AssetRegistry::GetDependencies(const AssetId& id) const
{
    std::shared_lock lock(m_Mutex);
    auto it = m_Dependencies.find(id);
    if (it == m_Dependencies.end()) return {};
    return std::vector<AssetId>(it->second.begin(), it->second.end());
}

std::vector<AssetId> AssetRegistry::GetDependents(const AssetId& id) const
{
    std::shared_lock lock(m_Mutex);
    auto it = m_Dependents.find(id);
    if (it == m_Dependents.end()) return {};
    return std::vector<AssetId>(it->second.begin(), it->second.end());
}

bool AssetRegistry::HasCircularDependency(const AssetId& asset, const AssetId& dependsOn) const
{
    // BFS from dependsOn to see if asset is reachable
    std::unordered_set<AssetId, AssetId::Hash> visited;
    std::vector<AssetId> queue = { dependsOn };

    while (!queue.empty())
    {
        AssetId current = queue.back();
        queue.pop_back();

        if (current == asset)
            return true;

        if (!visited.insert(current).second)
            continue;

        auto it = m_Dependencies.find(current);
        if (it != m_Dependencies.end())
        {
            for (const AssetId& dep : it->second)
                queue.push_back(dep);
        }
    }
    return false;
}

// =============================================================================
// Change Notification
// =============================================================================
void AssetRegistry::RegisterChangeCallback(const AssetId& id, AssetChangeCallback callback)
{
    std::unique_lock lock(m_Mutex);
    m_ChangeCallbacks[id].push_back(std::move(callback));
}

void AssetRegistry::NotifyChanged(const AssetId& id)
{
    std::vector<AssetChangeCallback> callbacks;
    {
        std::shared_lock lock(m_Mutex);
        auto it = m_ChangeCallbacks.find(id);
        if (it != m_ChangeCallbacks.end())
            callbacks = it->second;
    }
    for (auto& cb : callbacks)
        cb(id);
}

// =============================================================================
// Queries
// =============================================================================
size_t AssetRegistry::GetAssetCount() const
{
    std::shared_lock lock(m_Mutex);
    return m_Metadata.size();
}

std::vector<AssetId> AssetRegistry::GetAssetsByType(AssetType type) const
{
    std::shared_lock lock(m_Mutex);
    std::vector<AssetId> result;
    for (const auto& [id, meta] : m_Metadata)
        if (meta->type == type)
            result.push_back(id);
    return result;
}

bool AssetRegistry::HasAsset(const AssetId& id) const
{
    std::shared_lock lock(m_Mutex);
    return m_Metadata.find(id) != m_Metadata.end();
}

} // namespace caustica

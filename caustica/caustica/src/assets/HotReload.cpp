#include <assets/HotReload.h>

#include <core/PathUtils.h>

#include <mutex>

namespace caustica
{

void HotReloadTracker::watch(AssetId asset, const std::filesystem::path& path)
{
    if (!asset || path.empty())
        return;

    WatchedFile watched;
    const CanonicalAssetIdentity identity = canonicalizeAssetPath(path);
    watched.path = identity.path;
    watched.key = identity.key;
    if (std::filesystem::exists(watched.path))
    {
        watched.lastWriteTime = std::filesystem::last_write_time(watched.path);
        watched.hasTimestamp = true;
    }

    std::unique_lock lock(m_mutex);
    m_watchedFiles[asset] = std::move(watched);
}

void HotReloadTracker::unwatch(AssetId asset)
{
    std::unique_lock lock(m_mutex);
    m_watchedFiles.erase(asset);
}

std::vector<HotReloadChange> HotReloadTracker::pollChangedFiles()
{
    std::vector<HotReloadChange> changes;

    std::unique_lock lock(m_mutex);
    for (auto& [asset, watched] : m_watchedFiles)
    {
        if (m_ownedWrites.contains(watched.key))
            continue;
        if (!std::filesystem::exists(watched.path))
            continue;

        const auto currentWriteTime = std::filesystem::last_write_time(watched.path);
        if (!watched.hasTimestamp)
        {
            watched.lastWriteTime = currentWriteTime;
            watched.hasTimestamp = true;
            continue;
        }

        if (currentWriteTime != watched.lastWriteTime)
        {
            watched.lastWriteTime = currentWriteTime;
            changes.push_back(HotReloadChange{ asset, watched.path });
        }
    }

    return changes;
}

void HotReloadTracker::beginOwnedWrite(const std::filesystem::path& path)
{
    if (path.empty())
        return;
    const std::string key = canonicalAssetKey(path);
    std::unique_lock lock(m_mutex);
    ++m_ownedWrites[key];
}

void HotReloadTracker::endOwnedWrite(const std::filesystem::path& path)
{
    if (path.empty())
        return;
    const std::string key = canonicalAssetKey(path);
    std::unique_lock lock(m_mutex);
    auto owned = m_ownedWrites.find(key);
    if (owned == m_ownedWrites.end())
        return;
    if (--owned->second != 0)
        return;

    const bool exists = std::filesystem::exists(path);
    const auto writeTime = exists ? std::filesystem::last_write_time(path)
                                  : std::filesystem::file_time_type{};
    for (auto& [asset, watched] : m_watchedFiles)
    {
        (void)asset;
        if (watched.key != key)
            continue;
        watched.lastWriteTime = writeTime;
        watched.hasTimestamp = exists;
    }
    m_ownedWrites.erase(owned);
}

void HotReloadTracker::clear()
{
    std::unique_lock lock(m_mutex);
    m_watchedFiles.clear();
    m_ownedWrites.clear();
}

} // namespace caustica

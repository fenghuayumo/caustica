#include <assets/HotReload.h>

#include <mutex>

namespace caustica
{

void HotReloadTracker::watch(AssetId asset, const std::filesystem::path& path)
{
    if (!asset || path.empty())
        return;

    WatchedFile watched;
    watched.path = std::filesystem::absolute(path);
    if (std::filesystem::exists(watched.path))
    {
        watched.lastWriteTime = std::filesystem::last_write_time(watched.path);
        watched.hasTimestamp = true;
    }

    std::unique_lock lock(m_Mutex);
    m_WatchedFiles[asset] = std::move(watched);
}

void HotReloadTracker::unwatch(AssetId asset)
{
    std::unique_lock lock(m_Mutex);
    m_WatchedFiles.erase(asset);
}

std::vector<HotReloadChange> HotReloadTracker::pollChangedFiles()
{
    std::vector<HotReloadChange> changes;

    std::unique_lock lock(m_Mutex);
    for (auto& [asset, watched] : m_WatchedFiles)
    {
        if (m_OwnedWrites.contains(watched.path.lexically_normal()))
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
    const std::filesystem::path absolutePath = std::filesystem::absolute(path).lexically_normal();
    std::unique_lock lock(m_Mutex);
    ++m_OwnedWrites[absolutePath];
}

void HotReloadTracker::endOwnedWrite(const std::filesystem::path& path)
{
    if (path.empty())
        return;
    const std::filesystem::path absolutePath = std::filesystem::absolute(path).lexically_normal();
    std::unique_lock lock(m_Mutex);
    auto owned = m_OwnedWrites.find(absolutePath);
    if (owned == m_OwnedWrites.end())
        return;
    if (--owned->second != 0)
        return;

    const bool exists = std::filesystem::exists(absolutePath);
    const auto writeTime = exists ? std::filesystem::last_write_time(absolutePath)
                                  : std::filesystem::file_time_type{};
    for (auto& [asset, watched] : m_WatchedFiles)
    {
        (void)asset;
        if (watched.path.lexically_normal() != absolutePath)
            continue;
        watched.lastWriteTime = writeTime;
        watched.hasTimestamp = exists;
    }
    m_OwnedWrites.erase(owned);
}

void HotReloadTracker::clear()
{
    std::unique_lock lock(m_Mutex);
    m_WatchedFiles.clear();
    m_OwnedWrites.clear();
}

} // namespace caustica

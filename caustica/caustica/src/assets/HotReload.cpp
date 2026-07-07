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

void HotReloadTracker::clear()
{
    std::unique_lock lock(m_Mutex);
    m_WatchedFiles.clear();
}

} // namespace caustica

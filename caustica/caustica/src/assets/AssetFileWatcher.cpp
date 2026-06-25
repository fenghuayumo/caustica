#include <assets/AssetFileWatcher.h>
#include <core/log.h>

#include <algorithm>

namespace caustica
{

// =============================================================================
// WatchFile
// =============================================================================
void AssetFileWatcher::WatchFile(const std::filesystem::path& path, FileChangeCallback callback)
{
    std::lock_guard lock(m_Mutex);

    std::string key = std::filesystem::absolute(path).string();
    WatchEntry& entry = m_Watches[key];
    entry.path = path;
    entry.isDirectory = false;
    entry.recursive = false;
    entry.callback = std::move(callback);

    // Initialize last-check time
    if (std::filesystem::exists(path))
        entry.lastCheck = std::filesystem::last_write_time(path);
}

// =============================================================================
// WatchDirectory
// =============================================================================
void AssetFileWatcher::WatchDirectory(const std::filesystem::path& path, bool recursive,
                                       FileChangeCallback callback)
{
    std::lock_guard lock(m_Mutex);

    std::string key = std::filesystem::absolute(path).string();
    WatchEntry& entry = m_Watches[key];
    entry.path = path;
    entry.isDirectory = true;
    entry.recursive = recursive;
    entry.callback = std::move(callback);
    entry.lastCheck = std::filesystem::file_time_type::clock::now();
}

// =============================================================================
// Unwatch
// =============================================================================
void AssetFileWatcher::Unwatch(const std::filesystem::path& path)
{
    std::lock_guard lock(m_Mutex);
    std::string key = std::filesystem::absolute(path).string();
    m_Watches.erase(key);
}

// =============================================================================
// Clear
// =============================================================================
void AssetFileWatcher::Clear()
{
    std::lock_guard lock(m_Mutex);
    m_Watches.clear();
}

// =============================================================================
// Update — poll for changes
// =============================================================================
void AssetFileWatcher::Update()
{
    if (!m_Enabled)
        return;

    auto now = std::chrono::steady_clock::now();
    if (m_LastPoll.time_since_epoch().count() > 0)
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_LastPoll);
        if (elapsed < std::chrono::milliseconds(500)) // poll every 500ms
            return;
    }
    m_LastPoll = now;

    std::lock_guard lock(m_Mutex);
    for (auto& [key, entry] : m_Watches)
        CheckEntry(entry);
}

// =============================================================================
// CheckEntry — check a single watch for changes
// =============================================================================
void AssetFileWatcher::CheckEntry(WatchEntry& entry)
{
    if (entry.isDirectory)
    {
        if (!std::filesystem::exists(entry.path))
            return;

        // Iterate directory contents
        for (const auto& dirEntry : std::filesystem::directory_iterator(entry.path))
        {
            if (!dirEntry.is_regular_file())
                continue;

            auto ftime = dirEntry.last_write_time();
            if (ftime > entry.lastCheck)
            {
                entry.lastCheck = std::filesystem::file_time_type::clock::now();

                FileChangeEvent event;
                event.path = dirEntry.path();
                event.type = FileChangeType::Modified;
                event.timestamp = ftime;
                entry.callback(event);
            }
        }
    }
    else
    {
        bool exists = std::filesystem::exists(entry.path);
        if (!exists)
            return;

        auto ftime = std::filesystem::last_write_time(entry.path);
        if (ftime != entry.lastCheck)
        {
            FileChangeEvent event;
            event.path = entry.path;
            event.type = FileChangeType::Modified;
            event.timestamp = ftime;

            entry.lastCheck = ftime;
            entry.callback(event);
        }
    }
}

} // namespace caustica

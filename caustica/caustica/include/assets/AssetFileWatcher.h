#pragma once

#include <assets/AssetId.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// =============================================================================
// AssetFileWatcher — poll-based filesystem monitor for hot-reload.
//
// Watches registered file paths and invokes callbacks when changes are
// detected. Uses polling (file_time_type comparison) rather than OS-level
// change notification, which works reliably across all platforms without
// requiring platform-specific APIs.
// =============================================================================

namespace caustica
{

enum class FileChangeType
{
    Modified,
    Created,
    Deleted
};

struct FileChangeEvent
{
    std::filesystem::path path;
    FileChangeType        type;
    std::filesystem::file_time_type timestamp;
};

using FileChangeCallback = std::function<void(const FileChangeEvent&)>;

class AssetFileWatcher
{
public:
    AssetFileWatcher() = default;
    ~AssetFileWatcher() = default;

    // Watch a specific file. Calls callback when modified/deleted.
    void WatchFile(const std::filesystem::path& path, FileChangeCallback callback);

    // Watch a directory recursively. Calls callback for any changed file.
    void WatchDirectory(const std::filesystem::path& path, bool recursive, FileChangeCallback callback);

    // Stop watching a path.
    void Unwatch(const std::filesystem::path& path);

    // Poll for changes. Call once per frame (or every few frames).
    void Update();

    // Enable/disable
    void SetEnabled(bool enabled) { m_Enabled = enabled; }
    [[nodiscard]] bool IsEnabled() const { return m_Enabled; }
    [[nodiscard]] size_t GetWatchCount() const { return m_Watches.size(); }

    // Clear all watches
    void Clear();

private:
    struct WatchEntry
    {
        std::filesystem::path               path;
        std::filesystem::file_time_type     lastCheck;
        bool                                isDirectory = false;
        bool                                recursive = false;
        FileChangeCallback                  callback;
    };

    mutable std::mutex m_Mutex;
    std::unordered_map<std::string, WatchEntry> m_Watches;
    bool m_Enabled = false;
    std::chrono::steady_clock::time_point m_LastPoll;

    void CheckEntry(WatchEntry& entry);
};

} // namespace caustica

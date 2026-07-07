#pragma once

#include <assets/AssetId.h>

#include <filesystem>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace caustica
{

struct HotReloadChange
{
    AssetId asset = AssetId::invalid();
    std::filesystem::path path;
};

class HotReloadTracker
{
public:
    void watch(AssetId asset, const std::filesystem::path& path);
    void unwatch(AssetId asset);
    [[nodiscard]] std::vector<HotReloadChange> pollChangedFiles();
    void clear();

private:
    struct WatchedFile
    {
        std::filesystem::path path;
        std::filesystem::file_time_type lastWriteTime{};
        bool hasTimestamp = false;
    };

    mutable std::shared_mutex m_Mutex;
    std::unordered_map<AssetId, WatchedFile, AssetId::Hash> m_WatchedFiles;
};

} // namespace caustica

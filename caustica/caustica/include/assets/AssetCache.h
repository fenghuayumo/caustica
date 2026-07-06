#pragma once

#include <assets/AssetId.h>

#include <functional>
#include <memory>
#include <shared_mutex>
#include <unordered_map>

namespace caustica
{

enum class CacheState : uint8_t
{
    Unloaded = 0,
    Loading  = 1,
    Loaded   = 2,
    Failed   = 3,
    Evicted  = 4,
};

template <typename AssetType>
class AssetCache
{
public:
    [[nodiscard]] std::shared_ptr<AssetType> getAny(const AssetId& id)
    {
        std::shared_lock lock(m_Mutex);
        if (auto it = m_Entries.find(id); it != m_Entries.end())
            return it->second.asset;
        return nullptr;
    }

    void insert(const AssetId& id, std::shared_ptr<AssetType> asset)
    {
        std::unique_lock lock(m_Mutex);
        Entry& entry = m_Entries[id];
        entry.asset = std::move(asset);
        entry.state = CacheState::Loaded;
    }

    void remove(const AssetId& id)
    {
        std::unique_lock lock(m_Mutex);
        m_Entries.erase(id);
    }

    template <typename F>
    void forEach(F&& func) const
    {
        std::shared_lock lock(m_Mutex);
        for (const auto& [id, entry] : m_Entries)
            func(id, entry.asset, entry.state);
    }

    void clear()
    {
        std::unique_lock lock(m_Mutex);
        m_Entries.clear();
    }

private:
    struct Entry
    {
        std::shared_ptr<AssetType> asset;
        CacheState state = CacheState::Unloaded;
    };

    mutable std::shared_mutex m_Mutex;
    std::unordered_map<AssetId, Entry, AssetId::Hash> m_Entries;
};

} // namespace caustica

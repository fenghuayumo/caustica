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
        std::shared_lock lock(m_mutex);
        if (auto it = m_entries.find(id); it != m_entries.end())
            return it->second.asset;
        return nullptr;
    }

    void insert(const AssetId& id, std::shared_ptr<AssetType> asset)
    {
        std::unique_lock lock(m_mutex);
        Entry& entry = m_entries[id];
        entry.asset = std::move(asset);
        entry.state = CacheState::Loaded;
    }

    void remove(const AssetId& id)
    {
        std::unique_lock lock(m_mutex);
        m_entries.erase(id);
    }

    template <typename F>
    void forEach(F&& func) const
    {
        std::shared_lock lock(m_mutex);
        for (const auto& [id, entry] : m_entries)
            func(id, entry.asset, entry.state);
    }

    void clear()
    {
        std::unique_lock lock(m_mutex);
        m_entries.clear();
    }

private:
    struct Entry
    {
        std::shared_ptr<AssetType> asset;
        CacheState state = CacheState::Unloaded;
    };

    mutable std::shared_mutex m_mutex;
    std::unordered_map<AssetId, Entry, AssetId::Hash> m_entries;
};

} // namespace caustica

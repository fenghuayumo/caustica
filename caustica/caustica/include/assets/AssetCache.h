#pragma once

#include <assets/AssetId.h>
#include <core/Handle.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

// =============================================================================
// AssetCache<T> — CPU-side typed asset cache with LRU eviction.
//
// Each AssetSystem owns typed caches (texture, mesh, material, etc.).
// This replaces separate TextureLoader, MeshCache, etc. with a single
// template parameterized by asset type.
//
// Features:
//   - Thread-safe get/insert/evict
//   - LRU tracking for memory pressure
//   - Loading state tracking
//   - Soft eviction (GPU-uploaded assets can have CPU data freed)
// =============================================================================

namespace caustica
{

// Loading state for a cached asset
enum class CacheState : uint8_t
{
    Unloaded = 0,
    Loading  = 1,
    Loaded   = 2,   // CPU data available
    Failed   = 3,
    Evicted  = 4,   // CPU data freed (GPU data still valid)
};

template <typename AssetType>
class AssetCache
{
public:
    AssetCache() = default;
    ~AssetCache() = default;

    // --- Lookup ---
    [[nodiscard]] std::shared_ptr<AssetType> Get(const AssetId& id)
    {
        std::shared_lock lock(m_Mutex);
        auto it = m_Entries.find(id);
        if (it != m_Entries.end() && it->second.state == CacheState::Loaded)
            return it->second.asset;
        return nullptr;
    }

    [[nodiscard]] CacheState GetState(const AssetId& id) const
    {
        std::shared_lock lock(m_Mutex);
        auto it = m_Entries.find(id);
        return it != m_Entries.end() ? it->second.state : CacheState::Unloaded;
    }

    [[nodiscard]] bool IsLoaded(const AssetId& id) const
    {
        return GetState(id) == CacheState::Loaded;
    }

    // Get an asset regardless of its cache state (e.g. entries that are still
    // being finalized on the GPU). Use Get() when the caller requires a fully
    // loaded asset.
    [[nodiscard]] std::shared_ptr<AssetType> GetAny(const AssetId& id)
    {
        std::shared_lock lock(m_Mutex);
        auto it = m_Entries.find(id);
        if (it != m_Entries.end())
            return it->second.asset;
        return nullptr;
    }

    // --- Insert / Update ---
    void Insert(const AssetId& id, std::shared_ptr<AssetType> asset)
    {
        std::unique_lock lock(m_Mutex);
        Entry& entry = m_Entries[id];
        entry.asset = asset;
        entry.state = CacheState::Loaded;
        entry.memorySize = EstimateSize(*asset);
    }

    void SetState(const AssetId& id, CacheState state)
    {
        std::unique_lock lock(m_Mutex);
        auto it = m_Entries.find(id);
        if (it != m_Entries.end())
            it->second.state = state;
    }

    // --- Eviction ---
    bool Evict(const AssetId& id)
    {
        std::unique_lock lock(m_Mutex);
        auto it = m_Entries.find(id);
        if (it == m_Entries.end())
            return false;
        it->second.asset.reset();
        it->second.state = CacheState::Evicted;
        return true;
    }

    void Remove(const AssetId& id)
    {
        std::unique_lock lock(m_Mutex);
        m_Entries.erase(id);
    }

    // --- LRU Eviction (oldest entries first, up to targetBytes) ---
    size_t EvictToBudget(size_t targetBytes)
    {
        std::unique_lock lock(m_Mutex);
        size_t current = TotalMemoryLocked();
        if (current <= targetBytes)
            return 0;

        // Collect entries sorted by last access (oldest first)
        std::vector<std::pair<AssetId, uint64_t>> lru;
        for (auto& [id, entry] : m_Entries)
        {
            if (entry.state == CacheState::Loaded)
                lru.emplace_back(id, entry.lastAccessFrame);
        }
        std::sort(lru.begin(), lru.end(),
                  [](auto& a, auto& b) { return a.second < b.second; });

        size_t freed = 0;
        for (auto& [id, _] : lru)
        {
            if (current - freed <= targetBytes)
                break;
            auto it = m_Entries.find(id);
            if (it != m_Entries.end() && it->second.state == CacheState::Loaded)
            {
                freed += it->second.memorySize;
                it->second.asset.reset();
                it->second.state = CacheState::Evicted;
            }
        }
        return freed;
    }

    // --- Size ---
    [[nodiscard]] size_t GetCount() const
    {
        std::shared_lock lock(m_Mutex);
        return m_Entries.size();
    }

    [[nodiscard]] size_t GetLoadedCount() const
    {
        std::shared_lock lock(m_Mutex);
        size_t count = 0;
        for (auto& [id, entry] : m_Entries)
            if (entry.state == CacheState::Loaded) ++count;
        return count;
    }

    [[nodiscard]] size_t GetTotalMemory() const
    {
        std::shared_lock lock(m_Mutex);
        return TotalMemoryLocked();
    }

    // --- Iteration ---
    template <typename F>
    void ForEach(F&& func) const
    {
        std::shared_lock lock(m_Mutex);
        for (auto& [id, entry] : m_Entries)
            func(id, entry.asset, entry.state);
    }

    void Clear()
    {
        std::unique_lock lock(m_Mutex);
        m_Entries.clear();
    }

private:
    struct Entry
    {
        std::shared_ptr<AssetType> asset;
        CacheState state = CacheState::Unloaded;
        uint64_t   lastAccessFrame = 0;
        size_t     memorySize = 0;
    };

    mutable std::shared_mutex m_Mutex;
    std::unordered_map<AssetId, Entry, AssetId::Hash> m_Entries;

    size_t TotalMemoryLocked() const
    {
        size_t total = 0;
        for (auto& [id, entry] : m_Entries)
            if (entry.state == CacheState::Loaded)
                total += entry.memorySize;
        return total;
    }

    // Override this per-type to provide accurate memory estimation.
    static size_t EstimateSize(const AssetType& asset)
    {
        return sizeof(AssetType); // default: just the struct size
    }
};

} // namespace caustica

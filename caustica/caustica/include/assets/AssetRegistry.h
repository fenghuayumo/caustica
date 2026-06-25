#pragma once

#include <assets/AssetId.h>
#include <assets/AssetHandle.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace caustica
{

// Callback for asset changes (hot-reload).
using AssetChangeCallback = std::function<void(const AssetId&)>;

// =============================================================================
// AssetRegistry — central database for all asset metadata.
//
// Responsibilities:
//   - Asset ID generation (UUID-based)
//   - Path → ID mapping (canonical path lookup)
//   - Dependency tracking (which assets reference which)
//   - Version management (bumped on hot-reload)
//   - Lifecycle state tracking
// =============================================================================
class AssetRegistry
{
public:
    AssetRegistry() = default;
    ~AssetRegistry() = default;

    // --- Registration ---
    // Register a new asset or update existing. Returns the AssetId.
    AssetId Register(const std::filesystem::path& path, AssetType type);

    // Unregister an asset and all its dependencies.
    void Unregister(const AssetId& id);

    // --- Lookup ---
    [[nodiscard]] AssetId FindByPath(const std::filesystem::path& path) const;
    [[nodiscard]] std::filesystem::path GetPath(const AssetId& id) const;
    [[nodiscard]] std::shared_ptr<AssetMetadata> GetMetadata(const AssetId& id) const;
    [[nodiscard]] AssetType GetType(const AssetId& id) const;
    [[nodiscard]] AssetState GetState(const AssetId& id) const;
    [[nodiscard]] uint32_t GetVersion(const AssetId& id) const;

    // --- State Management ---
    void SetState(const AssetId& id, AssetState state);
    void IncrementVersion(const AssetId& id);

    // --- Dependency Tracking ---
    void AddDependency(const AssetId& asset, const AssetId& dependsOn);
    void RemoveDependency(const AssetId& asset, const AssetId& dependsOn);
    [[nodiscard]] std::vector<AssetId> GetDependencies(const AssetId& id) const;
    [[nodiscard]] std::vector<AssetId> GetDependents(const AssetId& id) const;

    // --- Change Notification ---
    void RegisterChangeCallback(const AssetId& id, AssetChangeCallback callback);
    void NotifyChanged(const AssetId& id);

    // --- Queries ---
    [[nodiscard]] size_t GetAssetCount() const;
    [[nodiscard]] std::vector<AssetId> GetAssetsByType(AssetType type) const;
    [[nodiscard]] bool HasAsset(const AssetId& id) const;

    // --- Typed Handles ---
    template <typename T>
    [[nodiscard]] AssetHandle<T> GetHandle(const AssetId& id) const
    {
        std::shared_lock lock(m_Mutex);
        auto it = m_Metadata.find(id);
        if (it != m_Metadata.end())
            return AssetHandle<T>(id, it->second->version);
        return {};
    }

private:
    mutable std::shared_mutex m_Mutex;

    // Canonical path → AssetId
    std::unordered_map<std::string, AssetId> m_PathToId;

    // AssetId → Metadata
    std::unordered_map<AssetId, std::shared_ptr<AssetMetadata>, AssetId::Hash> m_Metadata;

    // AssetId → set of assets IT depends on
    std::unordered_map<AssetId, std::unordered_set<AssetId, AssetId::Hash>, AssetId::Hash> m_Dependencies;

    // AssetId → set of assets that depend on IT
    std::unordered_map<AssetId, std::unordered_set<AssetId, AssetId::Hash>, AssetId::Hash> m_Dependents;

    // Change callbacks
    std::unordered_map<AssetId, std::vector<AssetChangeCallback>, AssetId::Hash> m_ChangeCallbacks;

    // Check for circular dependencies
    bool HasCircularDependency(const AssetId& asset, const AssetId& dependsOn) const;
};

} // namespace caustica

#pragma once

#include <assets/AssetId.h>
#include <assets/Handle.h>

#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <utility>

namespace caustica
{

template <typename T>
class AssetStore
{
public:
    [[nodiscard]] Handle<T> insert(AssetId id, std::shared_ptr<T> asset)
    {
        std::unique_lock lock(m_mutex);
        m_assets[id] = asset;
        return Handle<T>(id, std::move(asset));
    }

    [[nodiscard]] Handle<T> handle(AssetId id) const
    {
        std::shared_lock lock(m_mutex);
        if (auto it = m_assets.find(id); it != m_assets.end())
            return Handle<T>(id, it->second);
        return {};
    }

    [[nodiscard]] std::shared_ptr<T> get(AssetId id) const
    {
        std::shared_lock lock(m_mutex);
        if (auto it = m_assets.find(id); it != m_assets.end())
            return it->second;
        return nullptr;
    }

    void remove(AssetId id)
    {
        std::unique_lock lock(m_mutex);
        m_assets.erase(id);
    }

    template <typename F>
    void forEach(F&& func) const
    {
        std::shared_lock lock(m_mutex);
        for (const auto& [id, asset] : m_assets)
            func(id, asset);
    }

    void clear()
    {
        std::unique_lock lock(m_mutex);
        m_assets.clear();
    }

private:
    mutable std::shared_mutex m_mutex;
    std::unordered_map<AssetId, std::shared_ptr<T>, AssetId::Hash> m_assets;
};

} // namespace caustica

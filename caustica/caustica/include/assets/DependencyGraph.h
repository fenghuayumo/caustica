#pragma once

#include <assets/AssetId.h>

#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace caustica
{

class DependencyGraph
{
public:
    void addDependency(AssetId asset, AssetId dependency);
    void removeAsset(AssetId asset);
    void clear();

    [[nodiscard]] std::vector<AssetId> dependenciesOf(AssetId asset) const;
    [[nodiscard]] std::vector<AssetId> dependentsOf(AssetId dependency) const;

private:
    mutable std::shared_mutex m_Mutex;
    std::unordered_map<AssetId, std::unordered_set<AssetId, AssetId::Hash>, AssetId::Hash> m_Dependencies;
    std::unordered_map<AssetId, std::unordered_set<AssetId, AssetId::Hash>, AssetId::Hash> m_Dependents;
};

} // namespace caustica

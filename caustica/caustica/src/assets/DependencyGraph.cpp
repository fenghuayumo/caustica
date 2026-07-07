#include <assets/DependencyGraph.h>

#include <mutex>

namespace caustica
{

void DependencyGraph::addDependency(AssetId asset, AssetId dependency)
{
    if (!asset || !dependency || asset == dependency)
        return;

    std::unique_lock lock(m_Mutex);
    m_Dependencies[asset].insert(dependency);
    m_Dependents[dependency].insert(asset);
}

void DependencyGraph::removeAsset(AssetId asset)
{
    if (!asset)
        return;

    std::unique_lock lock(m_Mutex);

    if (auto depIt = m_Dependencies.find(asset); depIt != m_Dependencies.end())
    {
        for (AssetId dependency : depIt->second)
        {
            if (auto revIt = m_Dependents.find(dependency); revIt != m_Dependents.end())
            {
                revIt->second.erase(asset);
                if (revIt->second.empty())
                    m_Dependents.erase(revIt);
            }
        }
        m_Dependencies.erase(depIt);
    }

    if (auto dependentIt = m_Dependents.find(asset); dependentIt != m_Dependents.end())
    {
        for (AssetId dependent : dependentIt->second)
        {
            if (auto depIt = m_Dependencies.find(dependent); depIt != m_Dependencies.end())
            {
                depIt->second.erase(asset);
                if (depIt->second.empty())
                    m_Dependencies.erase(depIt);
            }
        }
        m_Dependents.erase(dependentIt);
    }
}

void DependencyGraph::clear()
{
    std::unique_lock lock(m_Mutex);
    m_Dependencies.clear();
    m_Dependents.clear();
}

std::vector<AssetId> DependencyGraph::dependenciesOf(AssetId asset) const
{
    std::shared_lock lock(m_Mutex);
    if (auto it = m_Dependencies.find(asset); it != m_Dependencies.end())
        return { it->second.begin(), it->second.end() };
    return {};
}

std::vector<AssetId> DependencyGraph::dependentsOf(AssetId dependency) const
{
    std::shared_lock lock(m_Mutex);
    if (auto it = m_Dependents.find(dependency); it != m_Dependents.end())
        return { it->second.begin(), it->second.end() };
    return {};
}

} // namespace caustica

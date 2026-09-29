#include <assets/DependencyGraph.h>

#include <mutex>

namespace caustica
{

void DependencyGraph::addDependency(AssetId asset, AssetId dependency)
{
    if (!asset || !dependency || asset == dependency)
        return;

    std::unique_lock lock(m_mutex);
    m_dependencies[asset].insert(dependency);
    m_dependents[dependency].insert(asset);
}

void DependencyGraph::removeAsset(AssetId asset)
{
    if (!asset)
        return;

    std::unique_lock lock(m_mutex);

    if (auto depIt = m_dependencies.find(asset); depIt != m_dependencies.end())
    {
        for (AssetId dependency : depIt->second)
        {
            if (auto revIt = m_dependents.find(dependency); revIt != m_dependents.end())
            {
                revIt->second.erase(asset);
                if (revIt->second.empty())
                    m_dependents.erase(revIt);
            }
        }
        m_dependencies.erase(depIt);
    }

    if (auto dependentIt = m_dependents.find(asset); dependentIt != m_dependents.end())
    {
        for (AssetId dependent : dependentIt->second)
        {
            if (auto depIt = m_dependencies.find(dependent); depIt != m_dependencies.end())
            {
                depIt->second.erase(asset);
                if (depIt->second.empty())
                    m_dependencies.erase(depIt);
            }
        }
        m_dependents.erase(dependentIt);
    }
}

void DependencyGraph::clear()
{
    std::unique_lock lock(m_mutex);
    m_dependencies.clear();
    m_dependents.clear();
}

std::vector<AssetId> DependencyGraph::dependenciesOf(AssetId asset) const
{
    std::shared_lock lock(m_mutex);
    if (auto it = m_dependencies.find(asset); it != m_dependencies.end())
        return { it->second.begin(), it->second.end() };
    return {};
}

std::vector<AssetId> DependencyGraph::dependentsOf(AssetId dependency) const
{
    std::shared_lock lock(m_mutex);
    if (auto it = m_dependents.find(dependency); it != m_dependents.end())
        return { it->second.begin(), it->second.end() };
    return {};
}

} // namespace caustica

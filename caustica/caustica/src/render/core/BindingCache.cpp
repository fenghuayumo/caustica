#include <render/core/BindingCache.h>

namespace caustica
{

caustica::rhi::BindingSetHandle BindingCache::getCachedBindingSet(const caustica::rhi::BindingSetDesc& desc, caustica::rhi::IBindingLayout* layout)
{
    size_t hash = 0;
    caustica::rhi::hash_combine(hash, desc);
    caustica::rhi::hash_combine(hash, layout);

    m_mutex.lock_shared();

    caustica::rhi::BindingSetHandle result = nullptr;
    auto it = m_bindingSets.find(hash);
    if (it != m_bindingSets.end())
        result = it->second;

    m_mutex.unlock_shared();

    if (result)
    {
        assert(result->getDesc());
        assert(*result->getDesc() == desc);
    }

    return result;
}

caustica::rhi::BindingSetHandle BindingCache::getOrCreateBindingSet(const caustica::rhi::BindingSetDesc& desc, caustica::rhi::IBindingLayout* layout)
{
    size_t hash = 0;
    caustica::rhi::hash_combine(hash, desc);
    caustica::rhi::hash_combine(hash, layout);

    m_mutex.lock_shared();

    caustica::rhi::BindingSetHandle result;
    auto it = m_bindingSets.find(hash);
    if (it != m_bindingSets.end())
        result = it->second;

    m_mutex.unlock_shared();

    if (!result)
    {
        m_mutex.lock();

        caustica::rhi::BindingSetHandle& entry = m_bindingSets[hash];
        if (!entry)
        {
            result = m_device->createBindingSet(desc, layout);
            entry = result;
        }
        else
            result = entry;

        m_mutex.unlock();
    }

    if (result)
    {
        assert(result->getDesc());
        assert(*result->getDesc() == desc);
    }

    return result;
}

void BindingCache::clear()
{
    m_mutex.lock();
    m_bindingSets.clear();
    m_mutex.unlock();
}

} // namespace caustica

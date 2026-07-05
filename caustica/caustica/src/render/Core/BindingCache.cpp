#include <render/Core/BindingCache.h>

namespace caustica
{

nvrhi::BindingSetHandle BindingCache::getCachedBindingSet(const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout)
{
    size_t hash = 0;
    nvrhi::hash_combine(hash, desc);
    nvrhi::hash_combine(hash, layout);

    m_mutex.lock_shared();

    nvrhi::BindingSetHandle result = nullptr;
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

nvrhi::BindingSetHandle BindingCache::getOrCreateBindingSet(const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout)
{
    size_t hash = 0;
    nvrhi::hash_combine(hash, desc);
    nvrhi::hash_combine(hash, layout);

    m_mutex.lock_shared();

    nvrhi::BindingSetHandle result;
    auto it = m_bindingSets.find(hash);
    if (it != m_bindingSets.end())
        result = it->second;

    m_mutex.unlock_shared();

    if (!result)
    {
        m_mutex.lock();

        nvrhi::BindingSetHandle& entry = m_bindingSets[hash];
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

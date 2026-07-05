#pragma once

#include <rhi/nvrhi.h>

#include <shared_mutex>
#include <unordered_map>

namespace caustica
{

/*
BindingCache maintains a dictionary that maps binding set descriptors
into actual binding set objects. The binding sets are created on demand when
getOrCreateBindingSet(...) is called and the requested binding set does not exist.
Created binding sets are stored for the lifetime of BindingCache, or until
clear() is called.

All BindingCache methods are thread-safe.
*/
class BindingCache
{
public:
    explicit BindingCache(nvrhi::IDevice* device)
        : m_device(device)
    {
    }

    nvrhi::BindingSetHandle getCachedBindingSet(const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout);
    nvrhi::BindingSetHandle getOrCreateBindingSet(const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout);
    void clear();

private:
    nvrhi::DeviceHandle m_device;
    std::unordered_map<size_t, nvrhi::BindingSetHandle> m_bindingSets;
    std::shared_mutex m_mutex;
};

} // namespace caustica

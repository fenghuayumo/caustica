#pragma once

#include <rhi/rhi.h>

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
    explicit BindingCache(caustica::rhi::IDevice* device)
        : m_device(device)
    {
    }

    caustica::rhi::BindingSetHandle getCachedBindingSet(const caustica::rhi::BindingSetDesc& desc, caustica::rhi::IBindingLayout* layout);
    caustica::rhi::BindingSetHandle getOrCreateBindingSet(const caustica::rhi::BindingSetDesc& desc, caustica::rhi::IBindingLayout* layout);
    void clear();

private:
    caustica::rhi::DeviceHandle m_device;
    std::unordered_map<size_t, caustica::rhi::BindingSetHandle> m_bindingSets;
    std::shared_mutex m_mutex;
};

} // namespace caustica

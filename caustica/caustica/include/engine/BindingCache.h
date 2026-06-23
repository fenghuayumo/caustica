#pragma once

#include <rhi/nvrhi.h>
#include <unordered_map>
#include <shared_mutex>

namespace caustica
{
    /*
    BindingCache maintains a dictionary that maps binding set descriptors
    into actual binding set objects. The binding sets are created on demand when 
    GetOrCreateBindingSet(...) is called and the requested binding set does not exist.
    Created binding sets are stored for the lifetime of BindingCache, or until
    Clear() is called.
    
    All BindingCache methods are thread-safe.
    */
    class BindingCache
    {
    private:
        nvrhi::DeviceHandle m_Device;
        std::unordered_map<size_t, nvrhi::BindingSetHandle> m_BindingSets;
        std::shared_mutex m_Mutex;

    public:
        BindingCache(nvrhi::IDevice* device)
            : m_Device(device)
        { }

        nvrhi::BindingSetHandle GetCachedBindingSet(const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout);
        nvrhi::BindingSetHandle GetOrCreateBindingSet(const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout* layout);
        void Clear();
    };

}

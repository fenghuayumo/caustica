#pragma once

#include <core/DescriptorHandle.h>
#include <backend/IDescriptorTableManager.h>
#include <rhi/rhi.h>
#include <unordered_map>
#include <memory>

namespace caustica
{

    class DescriptorTableManager : public IDescriptorTableManager, public std::enable_shared_from_this<DescriptorTableManager>
    {
    protected:
        // Custom hasher that doesn't look at the binding slot
        struct BindingSetItemHasher
        {
            std::size_t operator()(const caustica::rhi::BindingSetItem& item) const
            {
                size_t hash = 0;
                caustica::rhi::hash_combine(hash, item.resourceHandle);
                caustica::rhi::hash_combine(hash, item.type);
                caustica::rhi::hash_combine(hash, item.format);
                caustica::rhi::hash_combine(hash, item.dimension);
                caustica::rhi::hash_combine(hash, item.rawData[0]);
                caustica::rhi::hash_combine(hash, item.rawData[1]);
                return hash;
            }
        };

        // Custom equality tester that doesn't look at the binding slot
        struct BindingSetItemsEqual
        {
            bool operator()(const caustica::rhi::BindingSetItem& a, const caustica::rhi::BindingSetItem& b) const 
            {
                return a.resourceHandle == b.resourceHandle
                    && a.type == b.type
                    && a.format == b.format
                    && a.dimension == b.dimension
                    && a.subresources == b.subresources;
            }
        };
        
        caustica::rhi::DeviceHandle m_device;
        caustica::rhi::DescriptorTableHandle m_descriptorTable;

        std::vector<caustica::rhi::BindingSetItem> m_descriptors;
        std::unordered_map<caustica::rhi::BindingSetItem, DescriptorIndex, BindingSetItemHasher, BindingSetItemsEqual> m_descriptorIndexMap;
        std::vector<bool> m_allocatedDescriptors;
        int m_searchStart = 0;
        
    public:
        DescriptorTableManager(caustica::rhi::IDevice* device, caustica::rhi::IBindingLayout* layout);
        ~DescriptorTableManager();
        
        caustica::rhi::IDescriptorTable* getDescriptorTable() const override { return m_descriptorTable; }

        DescriptorIndex createDescriptor(caustica::rhi::BindingSetItem item) override;
        DescriptorHandle createDescriptorHandle(caustica::rhi::BindingSetItem item) override;
        caustica::rhi::BindingSetItem getDescriptor(DescriptorIndex index);
        void releaseDescriptor(DescriptorIndex index) override;
    };
}

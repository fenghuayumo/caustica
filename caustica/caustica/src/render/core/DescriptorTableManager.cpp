#include <render/core/DescriptorTableManager.h>
#include <core/DescriptorHandle.h>
#include <backend/IDescriptorTableManager.h>

caustica::DescriptorHandle::DescriptorHandle()
    : m_DescriptorIndex(-1)
{
}

caustica::DescriptorHandle::DescriptorHandle(const std::shared_ptr<IDescriptorTableManager>& managerPtr, DescriptorIndex index)
    : m_manager(managerPtr)
    , m_DescriptorIndex(index)
{
}

caustica::DescriptorHandle::~DescriptorHandle()
{
    if (m_DescriptorIndex >= 0)
    {
        auto managerPtr = m_manager.lock();
        if (managerPtr)
            managerPtr->releaseDescriptor(m_DescriptorIndex);
        m_DescriptorIndex = -1;
    }
}

caustica::DescriptorIndex caustica::DescriptorHandle::getIndexInHeap() const
{
    if (m_DescriptorIndex >= 0)
    {
        assert(!m_manager.expired());
        if (std::shared_ptr<IDescriptorTableManager> manager = m_manager.lock())
        {
            return manager->getDescriptorTable()->getFirstDescriptorIndexInHeap() + m_DescriptorIndex;
        }
    }
    return -1;
}

caustica::DescriptorTableManager::DescriptorTableManager(caustica::rhi::IDevice* device, caustica::rhi::IBindingLayout* layout)
    : m_device(device)
{
    m_descriptorTable = m_device->createDescriptorTable(layout);

    size_t capacity = m_descriptorTable->getCapacity();
    m_allocatedDescriptors.resize(capacity);
    m_descriptors.resize(capacity);
    memset(m_descriptors.data(), 0, sizeof(caustica::rhi::BindingSetItem) * capacity);
}

caustica::DescriptorIndex caustica::DescriptorTableManager::createDescriptor(caustica::rhi::BindingSetItem item)
{
    const auto& found = m_descriptorIndexMap.find(item);
    if (found != m_descriptorIndexMap.end())
        return found->second;

    uint32_t capacity = m_descriptorTable->getCapacity();
    bool foundFreeSlot = false;
    uint32_t index = 0;
    for (index = m_searchStart; index < capacity; index++)
    {
        if (!m_allocatedDescriptors[index])
        {
            foundFreeSlot = true;
            break;
        }
    }

    if (!foundFreeSlot)
    {
        uint32_t newCapacity = std::max(64u, capacity * 2); // handle the initial case when capacity == 0
        m_device->resizeDescriptorTable(m_descriptorTable, newCapacity);
        m_allocatedDescriptors.resize(newCapacity);
        m_descriptors.resize(newCapacity);

        // zero-fill the new descriptors
        memset(&m_descriptors[capacity], 0, sizeof(caustica::rhi::BindingSetItem) * (newCapacity - capacity));
        
        index = capacity;
        capacity = newCapacity;
    }

    item.slot = index;
    m_searchStart = index + 1;
    m_allocatedDescriptors[index] = true;
    m_descriptors[index] = item;
    m_descriptorIndexMap[item] = index;
    m_device->writeDescriptorTable(m_descriptorTable, item);

    if (item.resourceHandle)
        item.resourceHandle->AddRef();

    return index;
}

caustica::DescriptorHandle caustica::DescriptorTableManager::createDescriptorHandle(caustica::rhi::BindingSetItem item)
{
    DescriptorIndex index = createDescriptor(item);
    return DescriptorHandle(shared_from_this(), index);
}

caustica::rhi::BindingSetItem caustica::DescriptorTableManager::getDescriptor(DescriptorIndex index)
{
    if (size_t(index) >= m_descriptors.size())
        return caustica::rhi::BindingSetItem::None(0);

    return m_descriptors[index];
}

void caustica::DescriptorTableManager::releaseDescriptor(DescriptorIndex index)
{
    caustica::rhi::BindingSetItem& descriptor = m_descriptors[index];

    if (descriptor.resourceHandle)
        descriptor.resourceHandle->Release();

    // Erase the existing descriptor from the index map to prevent its "reuse" later
    const auto indexMapEntry = m_descriptorIndexMap.find(m_descriptors[index]);
    if (indexMapEntry != m_descriptorIndexMap.end())
        m_descriptorIndexMap.erase(indexMapEntry);

    descriptor = caustica::rhi::BindingSetItem::None(index);

    m_device->writeDescriptorTable(m_descriptorTable, descriptor);

    m_allocatedDescriptors[index] = false;
    m_searchStart = std::min(m_searchStart, index);
}

caustica::DescriptorTableManager::~DescriptorTableManager()
{
    for (auto& descriptor : m_descriptors)
    {
        if (descriptor.resourceHandle)
        {
            descriptor.resourceHandle->Release();
            descriptor.resourceHandle = nullptr;
        }
    }
}

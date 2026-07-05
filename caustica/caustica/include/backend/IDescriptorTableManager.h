#pragma once

#include <core/DescriptorHandle.h>
#include <rhi/nvrhi.h>
#include <memory>

namespace caustica
{

// Bindless descriptor allocation surface. Implemented by render::DescriptorTableManager.
class IDescriptorTableManager
{
public:
    virtual ~IDescriptorTableManager() = default;

    virtual nvrhi::IDescriptorTable* getDescriptorTable() const = 0;
    virtual DescriptorIndex createDescriptor(nvrhi::BindingSetItem item) = 0;
    virtual DescriptorHandle createDescriptorHandle(nvrhi::BindingSetItem item) = 0;
    virtual void releaseDescriptor(DescriptorIndex index) = 0;
};

} // namespace caustica

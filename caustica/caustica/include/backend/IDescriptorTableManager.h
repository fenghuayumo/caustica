#pragma once

#include <core/DescriptorHandle.h>
#include <rhi/rhi.h>
#include <memory>

namespace caustica
{

// Bindless descriptor allocation surface. Implemented by render::DescriptorTableManager.
class IDescriptorTableManager
{
public:
    virtual ~IDescriptorTableManager() = default;

    virtual caustica::rhi::DescriptorTable* getDescriptorTable() const = 0;
    virtual DescriptorIndex createDescriptor(caustica::rhi::BindingSetItem item) = 0;
    virtual DescriptorHandle createDescriptorHandle(caustica::rhi::BindingSetItem item) = 0;
    virtual void releaseDescriptor(DescriptorIndex index) = 0;
};

} // namespace caustica

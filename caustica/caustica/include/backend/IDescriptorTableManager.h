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

    virtual nvrhi::IDescriptorTable* GetDescriptorTable() const = 0;
    virtual DescriptorIndex CreateDescriptor(nvrhi::BindingSetItem item) = 0;
    virtual DescriptorHandle CreateDescriptorHandle(nvrhi::BindingSetItem item) = 0;
    virtual void ReleaseDescriptor(DescriptorIndex index) = 0;
};

} // namespace caustica

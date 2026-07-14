#pragma once

#include <cassert>
#include <memory>

// =============================================================================
// DescriptorHandle — lightweight bindless resource handle.
//
// Lives in core/ so both scene/ and render/ layers can use it without creating
// a circular dependency. The implementation (which needs DescriptorTableManager)
// lives in src/render/core/DescriptorTableManager.cpp.
// =============================================================================

namespace caustica
{

class IDescriptorTableManager;
typedef int DescriptorIndex;

class DescriptorHandle
{
private:
    std::weak_ptr<IDescriptorTableManager> m_manager;
    DescriptorIndex m_DescriptorIndex;

public:
    DescriptorHandle();
    DescriptorHandle(const std::shared_ptr<IDescriptorTableManager>& managerPtr, DescriptorIndex index);
    ~DescriptorHandle();

    [[nodiscard]] bool isValid() const { return m_DescriptorIndex >= 0 && !m_manager.expired(); }
    [[nodiscard]] DescriptorIndex Get() const { if (m_DescriptorIndex >= 0) assert(!m_manager.expired()); return m_DescriptorIndex; }
    [[nodiscard]] DescriptorIndex getIndexInHeap() const;
    void reset() { m_DescriptorIndex = -1; m_manager.reset(); }

    DescriptorHandle(const DescriptorHandle&) = delete;
    DescriptorHandle(DescriptorHandle&&) = default;
    DescriptorHandle& operator=(const DescriptorHandle&) = delete;
    DescriptorHandle& operator=(DescriptorHandle&&) = default;
};

} // namespace caustica

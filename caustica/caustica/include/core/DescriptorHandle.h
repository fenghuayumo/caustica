#pragma once

#include <cassert>
#include <memory>

// =============================================================================
// DescriptorHandle — lightweight bindless resource handle.
//
// Lives in core/ so both scene/ and render/ layers can use it without creating
// a circular dependency. The implementation (which needs DescriptorTableManager)
// lives in src/render/Core/DescriptorTableManager.cpp.
// =============================================================================

namespace caustica
{

class DescriptorTableManager;
typedef int DescriptorIndex;

class DescriptorHandle
{
private:
    std::weak_ptr<DescriptorTableManager> m_Manager;
    DescriptorIndex m_DescriptorIndex;

public:
    DescriptorHandle();
    DescriptorHandle(const std::shared_ptr<DescriptorTableManager>& managerPtr, DescriptorIndex index);
    ~DescriptorHandle();

    [[nodiscard]] bool IsValid() const { return m_DescriptorIndex >= 0 && !m_Manager.expired(); }
    [[nodiscard]] DescriptorIndex Get() const { if (m_DescriptorIndex >= 0) assert(!m_Manager.expired()); return m_DescriptorIndex; }
    [[nodiscard]] DescriptorIndex GetIndexInHeap() const;
    void Reset() { m_DescriptorIndex = -1; m_Manager.reset(); }

    DescriptorHandle(const DescriptorHandle&) = delete;
    DescriptorHandle(DescriptorHandle&&) = default;
    DescriptorHandle& operator=(const DescriptorHandle&) = delete;
    DescriptorHandle& operator=(DescriptorHandle&&) = default;
};

} // namespace caustica

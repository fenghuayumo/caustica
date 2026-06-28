#pragma once

#include <core/DescriptorHandle.h>
#include <render/Core/DescriptorTableManager.h>
#include <rhi/nvrhi.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

// =============================================================================
// BindlessHandle<Tag> - type-safe, generation-tracked bindless descriptor index.
//
// Encodes a 20-bit index and 12-bit generation counter into a uint32_t.
// The generation is bumped on free, so stale handles fail validation.
// =============================================================================

namespace caustica
{

struct BindlessTextureTag {};
struct BindlessBufferTag {};
struct BindlessSamplerTag {};

template <typename Tag>
class BindlessHandle
{
public:
    static constexpr uint32_t Invalid = 0xFFFFFFFF;

    BindlessHandle() = default;
    explicit BindlessHandle(uint32_t raw) : m_Value(raw) {}

    static BindlessHandle Make(uint32_t index, uint16_t generation)
    {
        return BindlessHandle((index & 0xFFFFF) | (uint32_t(generation) << 20));
    }

    [[nodiscard]] bool IsValid() const { return m_Value != Invalid; }
    [[nodiscard]] uint32_t GetIndex() const { return m_Value & 0xFFFFF; }
    [[nodiscard]] uint16_t GetGeneration() const { return uint16_t(m_Value >> 20); }
    [[nodiscard]] uint32_t GetRaw() const { return m_Value; }

    bool operator==(BindlessHandle o) const { return m_Value == o.m_Value; }
    bool operator!=(BindlessHandle o) const { return m_Value != o.m_Value; }

private:
    uint32_t m_Value = Invalid;
};

// Convenience aliases (prefixed to avoid nvrhi name clashes)
using BindlessTextureH  = BindlessHandle<BindlessTextureTag>;
using BindlessBufferH   = BindlessHandle<BindlessBufferTag>;
using BindlessSamplerH  = BindlessHandle<BindlessSamplerTag>;

// =============================================================================
// BindlessTable - O(1) descriptor allocation with generation tracking and
// deferred (frame-delayed) frees.
//
// Wraps the existing DescriptorTableManager with:
//   - Freelist-based allocation (no more linear scan)
//   - 12-bit generation counter per slot (bumped on free)
//   - Deferred-free queue (processed at end of frame, after GPU sync)
// =============================================================================

class BindlessTable
{
public:
    BindlessTable(nvrhi::IDevice* device, nvrhi::IBindingLayout* layout);
    ~BindlessTable();

    // --- New API: O(1) freelist allocation with generation tracking ---
    [[nodiscard]] uint32_t Allocate(nvrhi::BindingSetItem item);
    void Free(uint32_t handle);
    void FreeDeferred(uint32_t handle);
    void FlushDeferredFrees();
    [[nodiscard]] bool IsValid(uint32_t handle) const;

    // --- Backward-compatible API (replaces DescriptorTableManager) ---
    [[nodiscard]] DescriptorIndex CreateDescriptor(nvrhi::BindingSetItem item);
    [[nodiscard]] DescriptorHandle CreateDescriptorHandle(nvrhi::BindingSetItem item);
    void ReleaseDescriptor(DescriptorIndex index);
    [[nodiscard]] nvrhi::BindingSetItem GetDescriptor(DescriptorIndex index) const;

    // --- Accessors ---
    [[nodiscard]] nvrhi::IDescriptorTable* GetDescriptorTable() const;
    [[nodiscard]] std::shared_ptr<DescriptorTableManager> GetDescriptorTableManager() const { return m_Manager; }
    [[nodiscard]] uint32_t GetCapacity() const;
    [[nodiscard]] uint32_t GetAllocatedCount() const;

    // Create a typed bindless handle from a raw index.
    template <typename Tag>
    [[nodiscard]] BindlessHandle<Tag> MakeHandle(uint32_t index) const
    {
        if (index >= m_GenerationCount)
            return BindlessHandle<Tag>();
        return BindlessHandle<Tag>::Make(index, m_Generations[index].load());
    }

    // Fixed slots for common resources
    static constexpr uint32_t kWhiteTextureSlot = 3;
    static constexpr uint32_t kNormalTextureSlot = 4;

private:
    void Grow();

    std::shared_ptr<DescriptorTableManager> m_Manager;
    std::unique_ptr<std::atomic<uint16_t>[]> m_Generations;
    uint32_t                                 m_GenerationCount = 0;
    std::vector<uint32_t>                    m_FreeList;
    std::atomic<uint32_t>                    m_FreeCount{0};
    mutable std::mutex                       m_Mutex;
    std::queue<uint32_t>                     m_DeferredFrees;
    mutable std::mutex                       m_DeferredMutex;
};

} // namespace caustica

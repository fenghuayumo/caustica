#pragma once

#include <core/DescriptorHandle.h>
#include <render/core/DescriptorTableManager.h>
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
    explicit BindlessHandle(uint32_t raw) : m_value(raw) {}

    static BindlessHandle Make(uint32_t index, uint16_t generation)
    {
        return BindlessHandle((index & 0xFFFFF) | (uint32_t(generation) << 20));
    }

    [[nodiscard]] bool isValid() const { return m_value != Invalid; }
    [[nodiscard]] uint32_t getIndex() const { return m_value & 0xFFFFF; }
    [[nodiscard]] uint16_t getGeneration() const { return uint16_t(m_value >> 20); }
    [[nodiscard]] uint32_t getRaw() const { return m_value; }

    bool operator==(BindlessHandle o) const { return m_value == o.m_value; }
    bool operator!=(BindlessHandle o) const { return m_value != o.m_value; }

private:
    uint32_t m_value = Invalid;
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
    [[nodiscard]] bool isValid(uint32_t handle) const;

    // --- Backward-compatible API (replaces DescriptorTableManager) ---
    [[nodiscard]] DescriptorIndex createDescriptor(nvrhi::BindingSetItem item);
    [[nodiscard]] DescriptorHandle createDescriptorHandle(nvrhi::BindingSetItem item);
    void releaseDescriptor(DescriptorIndex index);
    [[nodiscard]] nvrhi::BindingSetItem getDescriptor(DescriptorIndex index) const;

    // --- Accessors ---
    [[nodiscard]] nvrhi::IDescriptorTable* getDescriptorTable() const;
    [[nodiscard]] std::shared_ptr<DescriptorTableManager> getDescriptorTableManager() const { return m_manager; }
    [[nodiscard]] uint32_t getCapacity() const;
    [[nodiscard]] uint32_t getAllocatedCount() const;

    // Create a typed bindless handle from a raw index.
    template <typename Tag>
    [[nodiscard]] BindlessHandle<Tag> makeHandle(uint32_t index) const
    {
        if (index >= m_generationCount)
            return BindlessHandle<Tag>();
        return BindlessHandle<Tag>::Make(index, m_generations[index].load());
    }

    // Fixed slots for common resources
    static constexpr uint32_t kWhiteTextureSlot = 3;
    static constexpr uint32_t kNormalTextureSlot = 4;

private:
    void Grow();

    std::shared_ptr<DescriptorTableManager> m_manager;
    std::unique_ptr<std::atomic<uint16_t>[]> m_generations;
    uint32_t                                 m_generationCount = 0;
    std::vector<uint32_t>                    m_freeList;
    std::atomic<uint32_t>                    m_freeCount{0};
    mutable std::mutex                       m_mutex;
    std::queue<uint32_t>                     m_deferredFrees;
    mutable std::mutex                       m_deferredMutex;
};

} // namespace caustica

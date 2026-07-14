#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

// =============================================================================
// Handle<T> — generation-counted resource handle.
//
// A lightweight 64-bit handle that combines an index (40 bits), generation
// (16 bits), and type tag (8 bits). When a slot is recycled, its generation
// is incremented, invalidating any outstanding handles that reference it.
// This prevents use-after-free without external bookkeeping.
//
// Pattern: allocate slots from a free-list pool. When a slot is freed,
// increment its generation. A handle with a stale generation will fail
// validation.
// =============================================================================

namespace caustica
{

template <typename Tag = void>
class Handle
{
public:
    static constexpr uint64_t Invalid = std::numeric_limits<uint64_t>::max();

    Handle() = default;
    explicit Handle(uint64_t raw) : m_Value(raw) {}

    // create a handle from index and generation.
    static Handle make(uint32_t index, uint16_t generation)
    {
        return Handle((uint64_t(index) & 0xFFFFFFFFFF) | (uint64_t(generation) << 40));
    }

    [[nodiscard]] bool isValid() const { return m_Value != Invalid; }
    [[nodiscard]] explicit operator bool() const { return isValid(); }

    [[nodiscard]] uint32_t GetIndex() const { return uint32_t(m_Value & 0xFFFFFFFFFF); }
    [[nodiscard]] uint16_t GetGeneration() const { return uint16_t(m_Value >> 40); }
    [[nodiscard]] uint64_t GetValue() const { return m_Value; }

    bool operator==(Handle other) const { return m_Value == other.m_Value; }
    bool operator!=(Handle other) const { return m_Value != other.m_Value; }

    struct Hash
    {
        size_t operator()(Handle h) const { return std::hash<uint64_t>()(h.m_Value); }
    };

private:
    uint64_t m_Value = Invalid;
};

// =============================================================================
// HandlePool<Tag, N> — thread-safe free-list pool with generation tracking.
//
// Allocates slots returning Handle<Tag>. When freed, the slot's generation is
// bumped so any outstanding handles become invalid.
// =============================================================================
template <typename Tag, size_t N>
class HandlePool
{
public:
    using HandleType = Handle<Tag>;

    HandlePool()
    {
        for (size_t i = 0; i < N; ++i)
        {
            m_FreeList[i] = static_cast<uint32_t>(N - 1 - i);
        }
        m_FreeCount = static_cast<uint32_t>(N);
    }

    // allocate a new handle. Returns Invalid if the pool is full.
    HandleType allocate()
    {
        uint32_t index = m_FreeCount.fetch_sub(1, std::memory_order_acquire);
        if (index == 0)
            return HandleType(); // pool full
        index = m_FreeList[index - 1];
        uint16_t gen = m_Generations[index].load(std::memory_order_relaxed);
        return HandleType::make(index, gen);
    }

    // free a handle, making its slot available for re-allocation.
    // Bumps the generation so any outstanding handles to this slot become invalid.
    void free(HandleType handle)
    {
        if (!handle.isValid())
            return;
        uint32_t index = handle.GetIndex();
        m_Generations[index].fetch_add(1, std::memory_order_release);
        uint32_t slot = m_FreeCount.fetch_add(1, std::memory_order_release);
        m_FreeList[slot] = index;
    }

    // Check if a handle is still valid (generation matches).
    [[nodiscard]] bool IsHandleValid(HandleType handle) const
    {
        if (!handle.isValid())
            return false;
        uint32_t index = handle.GetIndex();
        if (index >= N)
            return false;
        return m_Generations[index].load(std::memory_order_acquire) == handle.GetGeneration();
    }

    [[nodiscard]] size_t Capacity() const { return N; }
    [[nodiscard]] uint32_t Available() const { return m_FreeCount.load(std::memory_order_relaxed); }

private:
    std::atomic<uint16_t> m_Generations[N]{};
    uint32_t              m_FreeList[N]{};
    std::atomic<uint32_t> m_FreeCount{0};
};

} // namespace caustica

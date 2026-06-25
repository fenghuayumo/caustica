#include <render/Core/BindlessTable.h>
#include <render/Core/DescriptorTableManager.h>
#include <core/log.h>

#include <algorithm>

namespace caustica
{

// =============================================================================
// Constructor
// =============================================================================
BindlessTable::BindlessTable(nvrhi::IDevice* device, nvrhi::IBindingLayout* layout)
    : m_Manager(std::make_shared<DescriptorTableManager>(device, layout))
{
    uint32_t capacity = m_Manager->GetDescriptorTable()->getCapacity();
    m_Generations = std::make_unique<std::atomic<uint16_t>[]>(capacity);
    m_GenerationCount = capacity;
    m_FreeList.resize(capacity);

    // Initialize freelist: [capacity-1, capacity-2, ..., 0]
    for (uint32_t i = 0; i < capacity; ++i)
    {
        m_FreeList[i] = capacity - 1 - i;
        m_Generations[i].store(1, std::memory_order_relaxed); // start at gen 1
    }
    m_FreeCount.store(capacity, std::memory_order_relaxed);

    caustica::debug("BindlessTable created with capacity %u", capacity);
}

BindlessTable::~BindlessTable()
{
    FlushDeferredFrees();
}

// =============================================================================
// Grow — double the capacity
// =============================================================================
void BindlessTable::Grow()
{
    uint32_t oldCapacity = m_GenerationCount;
    uint32_t newCapacity = std::max(64u, oldCapacity * 2);

    // Allocate new generation array and copy old values
    auto newGens = std::make_unique<std::atomic<uint16_t>[]>(newCapacity);
    for (uint32_t i = 0; i < oldCapacity; ++i)
        newGens[i].store(m_Generations[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    for (uint32_t i = oldCapacity; i < newCapacity; ++i)
        newGens[i].store(1, std::memory_order_relaxed); // start at gen 1
    m_Generations = std::move(newGens);
    m_GenerationCount = newCapacity;

    m_FreeList.resize(newCapacity);

    // Add new slots to freelist (higher indices first for stack behavior)
    for (uint32_t i = oldCapacity; i < newCapacity; ++i)
    {
        m_FreeList[i] = i;
        m_Generations[i].store(1, std::memory_order_relaxed);
    }
    uint32_t oldFree = m_FreeCount.fetch_add(newCapacity - oldCapacity, std::memory_order_release);

    caustica::debug("BindlessTable grown from %u to %u (free: %u -> %u)",
                    oldCapacity, newCapacity, oldFree, oldFree + (newCapacity - oldCapacity));
}

// =============================================================================
// Allocate
// =============================================================================
uint32_t BindlessTable::Allocate(nvrhi::BindingSetItem item)
{
    std::lock_guard lock(m_Mutex);

    // If freelist is empty, grow
    uint32_t freeCount = m_FreeCount.load(std::memory_order_acquire);
    if (freeCount == 0)
    {
        Grow();
        freeCount = m_FreeCount.load(std::memory_order_acquire);
    }

    // Pop from freelist (stack-like: take last element)
    uint32_t slot = m_FreeList[freeCount - 1];
    m_FreeCount.store(freeCount - 1, std::memory_order_release);

    // Register with the underlying descriptor table
    m_Manager->CreateDescriptor(item);

    // Bump generation (on free, so next alloc gets a fresh generation)
    // Actually, generation is bumped on Free, not Allocate.
    // On allocate, we just return the current generation.

    uint16_t gen = m_Generations[slot].load(std::memory_order_relaxed);
    return BindlessHandle<BindlessTextureTag>::Make(slot, gen).GetRaw();
}

// =============================================================================
// Free
// =============================================================================
void BindlessTable::Free(uint32_t handle)
{
    BindlessHandle<BindlessTextureTag> h(handle);
    if (!h.IsValid())
        return;

    uint32_t index = h.GetIndex();
    if (index >= m_GenerationCount)
        return;

    // Bump generation (invalidates outstanding handles)
    m_Generations[index].fetch_add(1, std::memory_order_release);

    // Return to freelist
    std::lock_guard lock(m_Mutex);
    uint32_t freeCount = m_FreeCount.load(std::memory_order_relaxed);
    m_FreeList[freeCount] = index;
    m_FreeCount.store(freeCount + 1, std::memory_order_release);

    // Release from underlying descriptor table
    m_Manager->ReleaseDescriptor(index);
}

// =============================================================================
// FreeDeferred — queue for end-of-frame processing
// =============================================================================
void BindlessTable::FreeDeferred(uint32_t handle)
{
    std::lock_guard lock(m_DeferredMutex);
    m_DeferredFrees.push(handle);
}

// =============================================================================
// FlushDeferredFrees
// =============================================================================
void BindlessTable::FlushDeferredFrees()
{
    std::lock_guard lock(m_DeferredMutex);
    while (!m_DeferredFrees.empty())
    {
        uint32_t handle = m_DeferredFrees.front();
        m_DeferredFrees.pop();
        Free(handle);
    }
}

// =============================================================================
// IsValid — check generation
// =============================================================================
bool BindlessTable::IsValid(uint32_t handle) const
{
    BindlessHandle<BindlessTextureTag> h(handle);
    if (!h.IsValid()) return false;

    uint32_t index = h.GetIndex();
    if (index >= m_GenerationCount) return false;

    return m_Generations[index].load(std::memory_order_acquire) == h.GetGeneration();
}

// =============================================================================
// Accessors
// =============================================================================
nvrhi::IDescriptorTable* BindlessTable::GetDescriptorTable() const
{
    return m_Manager->GetDescriptorTable();
}

uint32_t BindlessTable::GetCapacity() const
{
    return m_Manager->GetDescriptorTable()->getCapacity();
}

uint32_t BindlessTable::GetAllocatedCount() const
{
    return GetCapacity() - m_FreeCount.load(std::memory_order_relaxed);
}

// =============================================================================
// Backward-compatible API — replaces DescriptorTableManager
// =============================================================================
DescriptorIndex BindlessTable::CreateDescriptor(nvrhi::BindingSetItem item)
{
    return static_cast<DescriptorIndex>(Allocate(item));
}

DescriptorHandle BindlessTable::CreateDescriptorHandle(nvrhi::BindingSetItem item)
{
    DescriptorIndex index = CreateDescriptor(item);
    // Note: shared_from_this won't work (BindlessTable isn't shared_ptr-managed).
    // We store a raw descriptor index; the handle's destructor behavior differs.
    return DescriptorHandle(m_Manager, index);
}

void BindlessTable::ReleaseDescriptor(DescriptorIndex index)
{
    FreeDeferred(static_cast<uint32_t>(index));
}

nvrhi::BindingSetItem BindlessTable::GetDescriptor(DescriptorIndex index) const
{
    return m_Manager->GetDescriptor(index);
}

} // namespace caustica

#include <render/core/BindlessTable.h>
#include <render/core/DescriptorTableManager.h>
#include <core/log.h>

#include <algorithm>

namespace caustica
{

// =============================================================================
// Constructor
// =============================================================================
BindlessTable::BindlessTable(nvrhi::IDevice* device, nvrhi::IBindingLayout* layout)
    : m_manager(std::make_shared<DescriptorTableManager>(device, layout))
{
    uint32_t capacity = m_manager->getDescriptorTable()->getCapacity();
    m_generations = std::make_unique<std::atomic<uint16_t>[]>(capacity);
    m_generationCount = capacity;
    m_freeList.resize(capacity);

    // Initialize freelist: [capacity-1, capacity-2, ..., 0]
    for (uint32_t i = 0; i < capacity; ++i)
    {
        m_freeList[i] = capacity - 1 - i;
        m_generations[i].store(1, std::memory_order_relaxed); // start at gen 1
    }
    m_freeCount.store(capacity, std::memory_order_relaxed);

    caustica::debug("BindlessTable created with capacity %u", capacity);
}

BindlessTable::~BindlessTable()
{
    flushDeferredFrees();
}

// =============================================================================
// grow — double the capacity
// =============================================================================
void BindlessTable::grow()
{
    uint32_t oldCapacity = m_generationCount;
    uint32_t newCapacity = std::max(64u, oldCapacity * 2);

    // allocate new generation array and copy old values
    auto newGens = std::make_unique<std::atomic<uint16_t>[]>(newCapacity);
    for (uint32_t i = 0; i < oldCapacity; ++i)
        newGens[i].store(m_generations[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    for (uint32_t i = oldCapacity; i < newCapacity; ++i)
        newGens[i].store(1, std::memory_order_relaxed); // start at gen 1
    m_generations = std::move(newGens);
    m_generationCount = newCapacity;

    m_freeList.resize(newCapacity);

    // Add new slots to freelist (higher indices first for stack behavior)
    for (uint32_t i = oldCapacity; i < newCapacity; ++i)
    {
        m_freeList[i] = i;
        m_generations[i].store(1, std::memory_order_relaxed);
    }
    uint32_t oldFree = m_freeCount.fetch_add(newCapacity - oldCapacity, std::memory_order_release);

    caustica::debug("BindlessTable grown from %u to %u (free: %u -> %u)",
                    oldCapacity, newCapacity, oldFree, oldFree + (newCapacity - oldCapacity));
}

// =============================================================================
// allocate
// =============================================================================
uint32_t BindlessTable::allocate(nvrhi::BindingSetItem item)
{
    std::lock_guard lock(m_mutex);

    // If freelist is empty, grow
    uint32_t freeCount = m_freeCount.load(std::memory_order_acquire);
    if (freeCount == 0)
    {
        grow();
        freeCount = m_freeCount.load(std::memory_order_acquire);
    }

    // Pop from freelist (stack-like: take last element)
    uint32_t slot = m_freeList[freeCount - 1];
    m_freeCount.store(freeCount - 1, std::memory_order_release);

    // Register with the underlying descriptor table
    m_manager->createDescriptor(item);

    // Bump generation (on free, so next alloc gets a fresh generation)
    // Actually, generation is bumped on free, not allocate.
    // On allocate, we just return the current generation.

    uint16_t gen = m_generations[slot].load(std::memory_order_relaxed);
    return BindlessHandle<BindlessTextureTag>::make(slot, gen).getRaw();
}

// =============================================================================
// free
// =============================================================================
void BindlessTable::free(uint32_t handle)
{
    BindlessHandle<BindlessTextureTag> h(handle);
    if (!h.isValid())
        return;

    uint32_t index = h.getIndex();
    if (index >= m_generationCount)
        return;

    // Bump generation (invalidates outstanding handles)
    m_generations[index].fetch_add(1, std::memory_order_release);

    // Return to freelist
    std::lock_guard lock(m_mutex);
    uint32_t freeCount = m_freeCount.load(std::memory_order_relaxed);
    m_freeList[freeCount] = index;
    m_freeCount.store(freeCount + 1, std::memory_order_release);

    // Release from underlying descriptor table
    m_manager->releaseDescriptor(index);
}

// =============================================================================
// freeDeferred — queue for end-of-frame processing
// =============================================================================
void BindlessTable::freeDeferred(uint32_t handle)
{
    std::lock_guard lock(m_deferredMutex);
    m_deferredFrees.push(handle);
}

// =============================================================================
// flushDeferredFrees
// =============================================================================
void BindlessTable::flushDeferredFrees()
{
    std::lock_guard lock(m_deferredMutex);
    while (!m_deferredFrees.empty())
    {
        uint32_t handle = m_deferredFrees.front();
        m_deferredFrees.pop();
        free(handle);
    }
}

// =============================================================================
// isValid — check generation
// =============================================================================
bool BindlessTable::isValid(uint32_t handle) const
{
    BindlessHandle<BindlessTextureTag> h(handle);
    if (!h.isValid()) return false;

    uint32_t index = h.getIndex();
    if (index >= m_generationCount) return false;

    return m_generations[index].load(std::memory_order_acquire) == h.getGeneration();
}

// =============================================================================
// Accessors
// =============================================================================
nvrhi::IDescriptorTable* BindlessTable::getDescriptorTable() const
{
    return m_manager->getDescriptorTable();
}

uint32_t BindlessTable::getCapacity() const
{
    return m_manager->getDescriptorTable()->getCapacity();
}

uint32_t BindlessTable::getAllocatedCount() const
{
    return getCapacity() - m_freeCount.load(std::memory_order_relaxed);
}

// =============================================================================
// Backward-compatible API — replaces DescriptorTableManager
// =============================================================================
DescriptorIndex BindlessTable::createDescriptor(nvrhi::BindingSetItem item)
{
    return static_cast<DescriptorIndex>(allocate(item));
}

DescriptorHandle BindlessTable::createDescriptorHandle(nvrhi::BindingSetItem item)
{
    DescriptorIndex index = createDescriptor(item);
    // Note: shared_from_this won't work (BindlessTable isn't shared_ptr-managed).
    // We store a raw descriptor index; the handle's destructor behavior differs.
    return DescriptorHandle(m_manager, index);
}

void BindlessTable::releaseDescriptor(DescriptorIndex index)
{
    freeDeferred(static_cast<uint32_t>(index));
}

nvrhi::BindingSetItem BindlessTable::getDescriptor(DescriptorIndex index) const
{
    return m_manager->getDescriptor(index);
}

} // namespace caustica

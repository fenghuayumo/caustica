#pragma once

#include <scene/SceneRenderData.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

namespace caustica
{

// Async structure GPU handoff owned by Scene (UE-style committed-serve while AS builds).
// Extract freezes / publishes; WorldRenderer serves committed proxies until finish.
class SceneStructureGpuSync
{
public:
    void requestGpuStructureSync();
    void clearGpuStructureSync();
    [[nodiscard]] bool needsGpuStructureSync() const { return m_pendingGpuStructureSync; }

    void freezeCommittedFromLogicCache(const scene::SceneRenderData& logicCache);
    void beginStructureGpuBuild();
    // Optionally replaces the committed serve packet, then clears in-flight.
    void finishStructureGpuBuild(std::shared_ptr<const scene::SceneRenderData> built = {});
    [[nodiscard]] bool structureGpuBuildInFlight() const
    {
        return m_structureGpuBuildInFlight.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::shared_ptr<const scene::SceneRenderData> committedRenderData() const;
    void acknowledgeGpuStructureConsumed(uint64_t publishedGeneration);

    [[nodiscard]] uint64_t publishedGeneration() const
    {
        return m_gpuStructureGeneration.load(std::memory_order_acquire);
    }
    [[nodiscard]] uint64_t consumedGeneration() const
    {
        return m_gpuStructureConsumedGeneration.load(std::memory_order_acquire);
    }

    // Called when Extract publishes a new structure generation.
    uint64_t bumpPublishedGeneration()
    {
        return m_gpuStructureGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    void setPublishedGeneration(uint64_t generation)
    {
        m_gpuStructureGeneration.store(generation, std::memory_order_release);
    }

private:
    std::atomic<uint64_t> m_gpuStructureGeneration{ 0 };
    std::atomic<uint64_t> m_gpuStructureConsumedGeneration{ 0 };
    bool m_pendingGpuStructureSync = false;
    std::atomic<bool> m_structureGpuBuildInFlight{ false };
    mutable std::mutex m_committedRenderDataMutex;
    std::shared_ptr<const scene::SceneRenderData> m_committedRenderData;
};

} // namespace caustica

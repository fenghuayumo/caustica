#include <scene/SceneStructureGpu.h>

#include <core/ThreadContext.h>

#include <utility>

namespace caustica
{

void SceneStructureGpuSync::requestGpuStructureSync()
{
    m_pendingGpuStructureSync = true;
}

void SceneStructureGpuSync::clearGpuStructureSync()
{
    m_pendingGpuStructureSync = false;
}

void SceneStructureGpuSync::freezeCommittedFromLogicCache(const scene::SceneRenderData& logicCache)
{
    assertLogicThread();
    auto frozen = std::make_shared<scene::SceneRenderData>(logicCache);
    std::lock_guard lock(m_committedRenderDataMutex);
    m_committedRenderData = std::move(frozen);
}

void SceneStructureGpuSync::beginStructureGpuBuild()
{
    m_structureGpuBuildInFlight.store(true, std::memory_order_release);
}

void SceneStructureGpuSync::finishStructureGpuBuild(std::shared_ptr<const scene::SceneRenderData> built)
{
    assertRenderThread();
    if (built)
    {
        std::lock_guard lock(m_committedRenderDataMutex);
        m_committedRenderData = std::move(built);
    }
    m_structureGpuBuildInFlight.store(false, std::memory_order_release);
}

std::shared_ptr<const scene::SceneRenderData> SceneStructureGpuSync::committedRenderData() const
{
    std::lock_guard lock(m_committedRenderDataMutex);
    return m_committedRenderData;
}

void SceneStructureGpuSync::acknowledgeGpuStructureConsumed(uint64_t publishedGeneration)
{
    uint64_t consumed = m_gpuStructureConsumedGeneration.load(std::memory_order_relaxed);
    while (consumed < publishedGeneration
        && !m_gpuStructureConsumedGeneration.compare_exchange_weak(
            consumed, publishedGeneration, std::memory_order_release, std::memory_order_relaxed))
    {
    }
}

} // namespace caustica

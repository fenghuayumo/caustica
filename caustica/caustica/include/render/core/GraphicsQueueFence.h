#pragma once

// ADR 0002 — wait the last graphics submit via EventQuery (not device-wide idle).

#include <rhi/rhi.h>

#include <core/log.h>

namespace caustica::render
{

// Ensures `query` exists, snapshots the last graphics submit, and CPU-waits it.
// Falls back to Device::waitForIdle only if EventQuery create fails.
// Returns false only on that fallback failing (device removed).
inline bool syncGraphicsQueueFence(
    caustica::rhi::Device* device,
    caustica::rhi::EventQueryHandle& query,
    bool runGc = false,
    const char* reason = nullptr)
{
    if (!device)
        return false;

    if (!query)
        query = device->createEventQuery();
    if (!query)
    {
        caustica::error(
            "syncGraphicsQueueFence: createEventQuery failed (%s); falling back to waitForIdle",
            reason ? reason : "graphics sync");
        // THREADING: sync-point, RT-only — ADR 0002 fallback.
        const bool ok = device->waitForIdle();
        if (runGc)
            device->runGarbageCollection();
        return ok;
    }

    // THREADING: queue fence, RT-only — ADR 0002.
    device->resetEventQuery(query);
    device->setEventQuery(query, caustica::rhi::CommandQueue::Graphics);
    device->waitEventQuery(query);
    device->resetEventQuery(query);
    if (runGc)
        device->runGarbageCollection();
    return true;
}

} // namespace caustica::render

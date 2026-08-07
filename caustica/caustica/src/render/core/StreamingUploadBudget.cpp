#include <render/core/StreamingUploadBudget.h>

#include <core/ThreadContext.h>
#include <core/log.h>

#include <algorithm>

namespace caustica::render
{

StreamingUploadBudget::StreamingUploadBudget(size_t maxInFlightBytes, uint32_t maxInFlightSubmits)
    : m_maxInFlightBytes(std::max<size_t>(maxInFlightBytes, 1))
    , m_maxInFlightSubmits(std::max<uint32_t>(maxInFlightSubmits, 1))
{
}

void StreamingUploadBudget::retireFront(caustica::rhi::Device* device)
{
    if (!device || m_entries.empty())
        return;

    Entry entry = std::move(m_entries.front());
    m_entries.pop_front();
    m_inFlightBytes -= std::min(m_inFlightBytes, entry.bytes);

    if (entry.query)
    {
        device->resetEventQuery(entry.query);
        m_queryPool.push_back(std::move(entry.query));
    }
}

void StreamingUploadBudget::retire(caustica::rhi::Device* device, bool runGc)
{
    caustica::assertRenderThread();
    if (!device)
        return;

    bool retiredAny = false;
    while (!m_entries.empty() && device->pollEventQuery(m_entries.front().query))
    {
        retireFront(device);
        retiredAny = true;
    }

    if (runGc && retiredAny)
        device->runGarbageCollection();
}

void StreamingUploadBudget::waitForBudget(caustica::rhi::Device* device, size_t nextBytes)
{
    caustica::assertRenderThread();
    if (!device)
        return;

    retire(device, /*runGc=*/true);

    const size_t effectiveNext = std::max<size_t>(nextBytes, 1);
    auto overBudget = [&]() {
        if (m_entries.size() >= m_maxInFlightSubmits)
            return true;
        // Oversized single submit: only allow when nothing else is in flight.
        if (effectiveNext >= m_maxInFlightBytes)
            return !m_entries.empty();
        return m_inFlightBytes + effectiveNext > m_maxInFlightBytes;
    };

    while (overBudget())
    {
        if (m_entries.empty())
            break;

        device->waitEventQuery(m_entries.front().query);
        retireFront(device);
        device->runGarbageCollection();
        retire(device, /*runGc=*/false);
    }
}

void StreamingUploadBudget::trackSubmit(
    caustica::rhi::Device* device,
    size_t bytes,
    caustica::rhi::CommandQueue queue)
{
    caustica::assertRenderThread();
    if (!device)
        return;

    caustica::rhi::EventQueryHandle query;
    if (!m_queryPool.empty())
    {
        query = std::move(m_queryPool.front());
        m_queryPool.pop_front();
    }
    else
    {
        query = device->createEventQuery();
    }

    if (!query)
    {
        caustica::error("StreamingUploadBudget: createEventQuery failed; falling back to waitForIdle");
        // THREADING: sync-point, RT-only — ADR 0001 R1 fallback (query create failed).
        if (!device->waitForIdle())
            caustica::error("StreamingUploadBudget: waitForIdle failed after query create failure");
        device->runGarbageCollection();
        return;
    }

    // setEventQuery snapshots the last submitted fence value — call after execute.
    device->resetEventQuery(query);
    device->setEventQuery(query, queue);

    Entry entry;
    entry.query = std::move(query);
    entry.bytes = std::max<size_t>(bytes, 1);
    m_inFlightBytes += entry.bytes;
    m_entries.push_back(std::move(entry));
}

void StreamingUploadBudget::waitAll(caustica::rhi::Device* device)
{
    caustica::assertRenderThread();
    if (!device)
        return;

    while (!m_entries.empty())
    {
        device->waitEventQuery(m_entries.front().query);
        retireFront(device);
    }
    device->runGarbageCollection();
}

StreamingUploadBudget& streamingUploadBudget()
{
    static StreamingUploadBudget budget;
    return budget;
}

} // namespace caustica::render

#pragma once

// ADR 0001 P4 R1 — fence-gated streaming uploads.
// Replaces per-batch Device::waitForIdle on the texture/mesh happy path with
// EventQuery completion + a capped in-flight DEFAULT/upload byte budget.

#include <rhi/rhi.h>

#include <cstddef>
#include <cstdint>
#include <deque>

namespace caustica::render
{

class StreamingUploadBudget
{
public:
    // Caps CreateCommittedResource + copy backlog so DWM stays responsive.
    static constexpr size_t kDefaultMaxInFlightBytes = 256ull * 1024ull * 1024ull;
    static constexpr uint32_t kDefaultMaxInFlightSubmits = 8;

    explicit StreamingUploadBudget(
        size_t maxInFlightBytes = kDefaultMaxInFlightBytes,
        uint32_t maxInFlightSubmits = kDefaultMaxInFlightSubmits);

    // Poll completed submits; optionally run GC when anything retires.
    void retire(caustica::rhi::Device* device, bool runGc = true);

    // CPU-wait oldest submits until nextBytes fits (or until empty if nextBytes > cap).
    void waitForBudget(caustica::rhi::Device* device, size_t nextBytes);

    // Call immediately after executeCommandList — snapshots the queue fence via EventQuery.
    void trackSubmit(
        caustica::rhi::Device* device,
        size_t bytes,
        caustica::rhi::CommandQueue queue = caustica::rhi::CommandQueue::Graphics);

    // Drain all tracked submits (finalize / loadingFinished / teardown).
    void waitAll(caustica::rhi::Device* device);

    [[nodiscard]] size_t inFlightBytes() const noexcept { return m_inFlightBytes; }
    [[nodiscard]] size_t inFlightCount() const noexcept { return m_entries.size(); }
    [[nodiscard]] size_t maxInFlightBytes() const noexcept { return m_maxInFlightBytes; }

private:
    struct Entry
    {
        caustica::rhi::EventQueryHandle query;
        size_t bytes = 0;
    };

    void retireFront(caustica::rhi::Device* device);

    size_t m_maxInFlightBytes = kDefaultMaxInFlightBytes;
    uint32_t m_maxInFlightSubmits = kDefaultMaxInFlightSubmits;
    size_t m_inFlightBytes = 0;
    std::deque<Entry> m_entries;
    std::deque<caustica::rhi::EventQueryHandle> m_queryPool;
};

// Render-thread process budget shared by TextureLoaderGpu + SceneGpuUpdater.
[[nodiscard]] StreamingUploadBudget& streamingUploadBudget();

} // namespace caustica::render

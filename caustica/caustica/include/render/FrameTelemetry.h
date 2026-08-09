#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace caustica::render
{

enum class FrameCpuStage : uint8_t
{
    Logic,
    Extract,
    FrameQueueWait,
    Render,
    Acquire,
    GraphBuild,
    GraphCompile,
    CommandRecord,
    Present,
    Count,
};

struct FrameGpuPassTiming
{
    std::string name;
    double milliseconds = 0.0;
};

struct FrameTelemetrySample
{
    uint32_t frameIndex = 0;
    bool valid = false;
    bool gpuValid = false;
    bool gpuPassesValid = false;
    std::array<double, static_cast<size_t>(FrameCpuStage::Count)> cpuMs{};
    double gpuMs = 0.0;
    std::vector<FrameGpuPassTiming> gpuPasses;
    uint32_t graphPasses = 0;
    uint32_t graphWaves = 0;
    uint32_t parallelBatches = 0;
    bool graphPlanCacheHit = false;
    bool presentHeadless = false;
    bool presentRequestedVsync = false;
    bool presentActiveVsync = false;
    bool presentWindowed = true;
    bool presentTearingSupported = false;
    bool presentTearingActive = false;
    uint32_t presentBackBufferCount = 0;

    [[nodiscard]] double cpu(FrameCpuStage stage) const
    {
        return cpuMs[static_cast<size_t>(stage)];
    }
};

// One bounded, thread-safe frame timing source shared by Logic, Render and UI.
// The ring is larger than the maximum frame latency, so a late GPU result can be
// attached to its original frame without allocating or publishing side channels.
class FrameTelemetry
{
public:
    static constexpr size_t kHistorySize = 64;

    void beginFrame(uint32_t frameIndex)
    {
        std::lock_guard lock(m_mutex);
        FrameTelemetrySample& sample = slot(frameIndex);
        if (!sample.valid || sample.frameIndex != frameIndex)
        {
            sample = {};
            sample.frameIndex = frameIndex;
            sample.valid = true;
        }
    }

    void addCpuTime(uint32_t frameIndex, FrameCpuStage stage, double milliseconds)
    {
        std::lock_guard lock(m_mutex);
        FrameTelemetrySample& sample = ensureSlot(frameIndex);
        sample.cpuMs[static_cast<size_t>(stage)] += milliseconds;
        publishLatest(frameIndex);
    }

    void setGpuTime(uint32_t frameIndex, double milliseconds)
    {
        std::lock_guard lock(m_mutex);
        FrameTelemetrySample& sample = ensureSlot(frameIndex);
        sample.gpuMs = milliseconds;
        sample.gpuValid = true;
        publishLatest(frameIndex);
    }

    void setGpuPassTimes(uint32_t frameIndex, std::vector<FrameGpuPassTiming> timings)
    {
        std::lock_guard lock(m_mutex);
        FrameTelemetrySample& sample = ensureSlot(frameIndex);
        sample.gpuPasses = std::move(timings);
        sample.gpuPassesValid = true;
        publishLatest(frameIndex);
    }

    void setGraphStats(
        uint32_t frameIndex,
        uint32_t passes,
        uint32_t waves,
        uint32_t parallelBatches,
        bool planCacheHit)
    {
        std::lock_guard lock(m_mutex);
        FrameTelemetrySample& sample = ensureSlot(frameIndex);
        sample.graphPasses = passes;
        sample.graphWaves = waves;
        sample.parallelBatches = parallelBatches;
        sample.graphPlanCacheHit = planCacheHit;
        publishLatest(frameIndex);
    }

    void setPresentStats(
        uint32_t frameIndex,
        bool headless,
        bool requestedVsync,
        bool activeVsync,
        bool windowed,
        bool tearingSupported,
        bool tearingActive,
        uint32_t backBufferCount)
    {
        std::lock_guard lock(m_mutex);
        FrameTelemetrySample& sample = ensureSlot(frameIndex);
        sample.presentHeadless = headless;
        sample.presentRequestedVsync = requestedVsync;
        sample.presentActiveVsync = activeVsync;
        sample.presentWindowed = windowed;
        sample.presentTearingSupported = tearingSupported;
        sample.presentTearingActive = tearingActive;
        sample.presentBackBufferCount = backBufferCount;
        publishLatest(frameIndex);
    }

    [[nodiscard]] FrameTelemetrySample latest() const
    {
        std::lock_guard lock(m_mutex);
        return m_hasLatest ? slot(m_latestFrameIndex) : FrameTelemetrySample{};
    }

    [[nodiscard]] FrameTelemetrySample latestRendered() const
    {
        std::lock_guard lock(m_mutex);
        return findLatest([](const FrameTelemetrySample& sample) {
            return sample.cpu(FrameCpuStage::Render) > 0.0;
        });
    }

    [[nodiscard]] FrameTelemetrySample latestGpu() const
    {
        std::lock_guard lock(m_mutex);
        return findLatest([](const FrameTelemetrySample& sample) { return sample.gpuValid; });
    }

    [[nodiscard]] FrameTelemetrySample latestGpuPasses() const
    {
        std::lock_guard lock(m_mutex);
        return findLatest([](const FrameTelemetrySample& sample) {
            return sample.gpuPassesValid;
        });
    }

private:
    [[nodiscard]] FrameTelemetrySample& slot(uint32_t frameIndex)
    {
        return m_history[frameIndex % kHistorySize];
    }

    [[nodiscard]] const FrameTelemetrySample& slot(uint32_t frameIndex) const
    {
        return m_history[frameIndex % kHistorySize];
    }

    FrameTelemetrySample& ensureSlot(uint32_t frameIndex)
    {
        FrameTelemetrySample& sample = slot(frameIndex);
        if (!sample.valid || sample.frameIndex != frameIndex)
        {
            sample = {};
            sample.frameIndex = frameIndex;
            sample.valid = true;
        }
        return sample;
    }

    void publishLatest(uint32_t frameIndex)
    {
        if (!m_hasLatest || static_cast<int32_t>(frameIndex - m_latestFrameIndex) >= 0)
        {
            m_latestFrameIndex = frameIndex;
            m_hasLatest = true;
        }
    }

    template<typename Predicate>
    [[nodiscard]] FrameTelemetrySample findLatest(Predicate predicate) const
    {
        if (!m_hasLatest)
            return {};
        for (uint32_t age = 0; age < uint32_t(kHistorySize); ++age)
        {
            const uint32_t frameIndex = m_latestFrameIndex - age;
            const FrameTelemetrySample& sample = slot(frameIndex);
            if (sample.valid && sample.frameIndex == frameIndex && predicate(sample))
                return sample;
        }
        return {};
    }

    mutable std::mutex m_mutex;
    std::array<FrameTelemetrySample, kHistorySize> m_history{};
    uint32_t m_latestFrameIndex = 0;
    bool m_hasLatest = false;
};

class ScopedFrameCpuTimer
{
public:
    ScopedFrameCpuTimer(FrameTelemetry* telemetry, uint32_t frameIndex, FrameCpuStage stage)
        : m_telemetry(telemetry)
        , m_frameIndex(frameIndex)
        , m_stage(stage)
        , m_begin(Clock::now())
    {
    }

    ~ScopedFrameCpuTimer()
    {
        if (!m_telemetry)
            return;
        const double milliseconds =
            std::chrono::duration<double, std::milli>(Clock::now() - m_begin).count();
        m_telemetry->addCpuTime(m_frameIndex, m_stage, milliseconds);
    }

    ScopedFrameCpuTimer(const ScopedFrameCpuTimer&) = delete;
    ScopedFrameCpuTimer& operator=(const ScopedFrameCpuTimer&) = delete;

private:
    using Clock = std::chrono::steady_clock;
    FrameTelemetry* m_telemetry = nullptr;
    uint32_t m_frameIndex = 0;
    FrameCpuStage m_stage = FrameCpuStage::Logic;
    Clock::time_point m_begin;
};

} // namespace caustica::render

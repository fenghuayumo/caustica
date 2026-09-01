#pragma once

#include <core/progress.h>
#include <render/FrameTelemetry.h>
#include <render/core/RtPipelineCache.h>

#include <chrono>
#include <atomic>
#include <mutex>
#include <utility>

namespace caustica::render
{

// Host-owned diagnostics consumed by the path tracer (progress UI, benchmarks).
struct AppDiagnostics
{
    FrameTelemetry frameTelemetry;
    ProgressBar progressInitializingRenderer;
    // RT scratch for OMM / opacity queue; Logic mirrors into LoadSession::secondaryStreaming.
    // Do not gate Open Scene on this: scene teardown cancels secondary streaming.
    std::atomic<bool> asyncLoadingInProgress{false};

    void publishPipelineStats(
        RtPipelineWarmupStatus warmup,
        RtPipelineCacheStats cacheStats)
    {
        std::lock_guard lock(m_pipelineMutex);
        m_rtPipelineWarmup = std::move(warmup);
        m_rtPipelineCacheStats = std::move(cacheStats);
    }

    [[nodiscard]] RtPipelineWarmupStatus pipelineWarmupStatus() const
    {
        std::lock_guard lock(m_pipelineMutex);
        return m_rtPipelineWarmup;
    }

    [[nodiscard]] RtPipelineCacheStats pipelineCacheStats() const
    {
        std::lock_guard lock(m_pipelineMutex);
        return m_rtPipelineCacheStats;
    }

    void resetBenchmark()
    {
        std::lock_guard lock(m_benchmarkMutex);
        m_benchStart = std::chrono::high_resolution_clock::now();
        m_benchLast = m_benchStart;
        m_benchFrames = 0;
    }

    void recordBenchmarkFrame()
    {
        std::lock_guard lock(m_benchmarkMutex);
        ++m_benchFrames;
        m_benchLast = std::chrono::high_resolution_clock::now();
    }

    [[nodiscard]] float averageBenchmarkFrameSeconds() const
    {
        std::lock_guard lock(m_benchmarkMutex);
        if (m_benchFrames == 0)
            return 0.0f;
        const std::chrono::duration<double> elapsed = m_benchLast - m_benchStart;
        return float(elapsed.count() / m_benchFrames);
    }

private:
    mutable std::mutex m_pipelineMutex;
    RtPipelineWarmupStatus m_rtPipelineWarmup{};
    RtPipelineCacheStats m_rtPipelineCacheStats{};

    mutable std::mutex m_benchmarkMutex;
    std::chrono::high_resolution_clock::time_point m_benchStart =
        std::chrono::high_resolution_clock::now();
    std::chrono::high_resolution_clock::time_point m_benchLast = m_benchStart;
    int m_benchFrames = 0;
};

} // namespace caustica::render

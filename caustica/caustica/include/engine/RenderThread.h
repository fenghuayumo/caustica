#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace caustica
{

struct RenderFrameCompletion
{
    bool success = true;
    double elapsedTime = 0.0;
    double curTime = 0.0;
};

// Dedicated OS thread that pumps Affinity::Render (ADR 0001).
// All Logic→RT work — frames, EnqueueRenderCommand, LoadSession stream steps —
// shares that single domain queue. RenderThread only owns wake / in-flight
// frame pacing / completion mailbox (no parallel m_queue).
class RenderThread
{
public:
    static constexpr size_t kMaxInFlightFrames = 2;

    RenderThread() = default;
    ~RenderThread();

    RenderThread(const RenderThread&) = delete;
    RenderThread& operator=(const RenderThread&) = delete;

    void start();
    void stop();

    [[nodiscard]] bool isRunning() const { return m_running.load(std::memory_order_acquire); }
    [[nodiscard]] bool isRenderThread() const;

    // Enqueue onto Affinity::Render. Blocks only when kMaxInFlightFrames are already queued or executing.
    void dispatch(std::function<void()> task);

    // Drain the render domain, then run synchronously on Affinity::Render (swap-chain resize).
    void dispatchAndWait(std::function<void()> task);

    void waitForIdle();

    // Wake the worker so it can pump Affinity::Render (task::setRenderWake).
    void wake();

    void notifyFrameCompleted(RenderFrameCompletion completion);
    [[nodiscard]] std::optional<RenderFrameCompletion> consumeCompletedFrame();

private:
    void threadMain();

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false;
    // Frame-pacing only (dispatch). LoadSession / other Affinity::Render tasks do not count.
    size_t m_inFlight = 0;

    std::mutex m_completionMutex;
    std::deque<RenderFrameCompletion> m_completedFrames;

    std::atomic<bool> m_running{false};
    std::atomic<std::thread::id> m_renderThreadId{};
};

[[nodiscard]] bool isRenderThread();

} // namespace caustica

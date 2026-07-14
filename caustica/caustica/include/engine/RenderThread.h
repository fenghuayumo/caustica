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

// Runs GPU-facing work on a dedicated thread. Frame work is double-buffered: the main
// thread may submit up to two frames without blocking while the render thread works.
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

    // Enqueue a frame task. Blocks only when kMaxInFlightFrames are already queued or executing.
    void dispatch(std::function<void()> task);

    // Drain in-flight frames, then run synchronously (used for swap-chain resize).
    void dispatchAndWait(std::function<void()> task);

    void waitForIdle();

    void notifyFrameCompleted(RenderFrameCompletion completion);
    [[nodiscard]] std::optional<RenderFrameCompletion> consumeCompletedFrame();

private:
    void threadMain();

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_queue;
    std::function<void()> m_syncTask;
    bool m_syncPending = false;
    bool m_syncDone = false;
    bool m_stop = false;
    size_t m_inFlight = 0;
    bool m_executing = false;

    std::mutex m_completionMutex;
    std::deque<RenderFrameCompletion> m_completedFrames;

    std::atomic<bool> m_running{false};
    std::atomic<std::thread::id> m_renderThreadId{};
};

[[nodiscard]] bool isRenderThread();

} // namespace caustica

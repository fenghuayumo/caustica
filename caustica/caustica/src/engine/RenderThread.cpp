#include <engine/RenderThread.h>
#include <core/ThreadContext.h>

#include <cassert>

namespace caustica
{

bool isRenderThread()
{
    return currentThreadDomain() == ThreadDomain::Render;
}

RenderThread::~RenderThread()
{
    stop();
}

void RenderThread::start()
{
    if (m_running.load(std::memory_order_acquire))
        return;

    {
        std::lock_guard lock(m_mutex);
        m_stop = false;
        m_queue.clear();
        m_syncPending = false;
        m_syncDone = false;
        m_syncTask = nullptr;
        m_inFlight = 0;
        m_executing = false;
    }

    {
        std::lock_guard lock(m_completionMutex);
        m_completedFrames.clear();
    }

    m_thread = std::thread([this] { threadMain(); });
    m_running.store(true, std::memory_order_release);
}

void RenderThread::stop()
{
    if (!m_running.load(std::memory_order_acquire))
        return;

    waitForIdle();

    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_cv.notify_one();

    if (m_thread.joinable())
        m_thread.join();

    m_running.store(false, std::memory_order_release);
    m_renderThreadId.store(std::thread::id{}, std::memory_order_release);
}

bool RenderThread::isRenderThread() const
{
    if (!m_running.load(std::memory_order_acquire))
        return false;
    return std::this_thread::get_id() == m_renderThreadId.load(std::memory_order_acquire);
}

void RenderThread::dispatch(std::function<void()> task)
{
    if (!task)
        return;

    if (!m_running.load(std::memory_order_acquire))
    {
        task();
        return;
    }

    if (isRenderThread())
    {
        task();
        return;
    }

    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [this] { return m_inFlight < kMaxInFlightFrames || m_stop; });
    if (m_stop)
        return;

    m_queue.push_back(std::move(task));
    ++m_inFlight;
    m_cv.notify_one();
}

void RenderThread::dispatchAndWait(std::function<void()> task)
{
    if (!task)
        return;

    if (!m_running.load(std::memory_order_acquire))
    {
        task();
        return;
    }

    if (isRenderThread())
    {
        task();
        return;
    }

    waitForIdle();

    std::unique_lock lock(m_mutex);
    assert(!m_syncPending && "RenderThread: nested dispatchAndWait is not supported");
    m_syncTask = std::move(task);
    m_syncDone = false;
    m_syncPending = true;
    m_cv.notify_one();
    m_cv.wait(lock, [this] { return m_syncDone; });
    m_syncTask = nullptr;
}

void RenderThread::waitForIdle()
{
    if (!m_running.load(std::memory_order_acquire))
        return;

    if (isRenderThread())
        return;

    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [this] {
        return m_queue.empty() && !m_executing && m_inFlight == 0 && !m_syncPending;
    });
}

void RenderThread::notifyFrameCompleted(RenderFrameCompletion completion)
{
    assert(isRenderThread() && "notifyFrameCompleted must be called on the render thread");
    std::lock_guard lock(m_completionMutex);
    m_completedFrames.push_back(completion);
}

std::optional<RenderFrameCompletion> RenderThread::consumeCompletedFrame()
{
    std::lock_guard lock(m_completionMutex);
    if (m_completedFrames.empty())
        return std::nullopt;
    RenderFrameCompletion completion = m_completedFrames.front();
    m_completedFrames.pop_front();
    return completion;
}

void RenderThread::threadMain()
{
    const ThreadDomainScope renderDomain(ThreadDomain::Render);
    m_renderThreadId.store(std::this_thread::get_id(), std::memory_order_release);

    std::unique_lock lock(m_mutex);
    while (true)
    {
        m_cv.wait(lock, [this] {
            return m_stop || m_syncPending || !m_queue.empty();
        });

        if (m_stop && !m_syncPending && m_queue.empty())
            break;

        if (m_syncPending)
        {
            std::function<void()> task = std::move(m_syncTask);
            m_syncPending = false;
            lock.unlock();

            task();

            lock.lock();
            m_syncDone = true;
            m_cv.notify_one();
            continue;
        }

        if (m_queue.empty())
            continue;

        std::function<void()> task = std::move(m_queue.front());
        m_queue.pop_front();
        m_executing = true;
        lock.unlock();

        task();

        lock.lock();
        m_executing = false;
        assert(m_inFlight > 0);
        --m_inFlight;
        m_cv.notify_all();
    }

    m_renderThreadId.store(std::thread::id{}, std::memory_order_release);
}

} // namespace caustica

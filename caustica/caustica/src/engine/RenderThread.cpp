#include <engine/RenderThread.h>
#include <core/ThreadContext.h>
#include <core/task/TaskRuntime.h>

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
        m_inFlight = 0;
    }

    {
        std::lock_guard lock(m_completionMutex);
        m_completedFrames.clear();
    }

    m_thread = std::thread([this] { threadMain(); });
    m_running.store(true, std::memory_order_release);

    task::setRenderWake([this]() { wake(); });
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
    task::setRenderWake(nullptr);
}

bool RenderThread::isRenderThread() const
{
    // Unified with caustica::isRenderThread() / assertRenderThread (ThreadDomain).
    return caustica::isRenderThread();
}

void RenderThread::wake()
{
    m_cv.notify_one();
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

    {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] { return m_inFlight < kMaxInFlightFrames || m_stop; });
        if (m_stop)
            return;
        ++m_inFlight;
    }

    task::TaskDesc desc;
    desc.name = "RenderThread.Dispatch";
    desc.priority = task::Priority::High;
    desc.affinity = task::Affinity::Render;
    desc.body = [this, body = std::move(task)]() {
        body();
        {
            std::lock_guard lock(m_mutex);
            assert(m_inFlight > 0);
            --m_inFlight;
        }
        m_cv.notify_all();
    };
    (void)task::launch(std::move(desc));
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

    task::TaskDesc desc;
    desc.name = "RenderThread.Sync";
    desc.priority = task::Priority::Critical;
    desc.affinity = task::Affinity::Render;
    desc.body = std::move(task);
    task::TaskHandle handle = task::launch(std::move(desc));
    task::wait(handle);
}

void RenderThread::waitForIdle()
{
    if (!m_running.load(std::memory_order_acquire))
        return;

    if (isRenderThread())
    {
        task::pumpRender();
        return;
    }

    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [this] {
        return m_inFlight == 0 && !task::isRenderDomainBusy();
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
            return m_stop || task::isRenderDomainBusy();
        });

        if (m_stop && !task::isRenderDomainBusy())
            break;

        lock.unlock();
        task::pumpRender();
        lock.lock();
        m_cv.notify_all();
    }

    task::pumpRender();
    m_renderThreadId.store(std::thread::id{}, std::memory_order_release);
}

} // namespace caustica

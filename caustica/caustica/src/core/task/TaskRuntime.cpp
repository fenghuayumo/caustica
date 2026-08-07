#include <core/task/TaskRuntime.h>
#include <core/log.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace caustica::task
{
namespace detail
{
struct TaskState;
}

class Pipe
{
public:
    explicit Pipe(std::string name)
        : m_name(std::move(name))
    {
    }

    void enqueue(std::shared_ptr<detail::TaskState> task);
    void onTaskFinished();
    [[nodiscard]] const std::string& name() const { return m_name; }

private:
    void scheduleNextUnlocked();

    std::string m_name;
    std::mutex m_mutex;
    std::deque<std::shared_ptr<detail::TaskState>> m_pending;
    bool m_running = false;
};

namespace detail
{

struct TaskState
{
    TaskFn fn = nullptr;
    void* user = nullptr;
    std::function<void()> body;
    const char* name = nullptr;
    Priority priority = Priority::Normal;
    Affinity affinity = Affinity::Any;
    Pipe* pipe = nullptr;
    uint64_t generation = 0;
    TaskDesc::GenerationDomain generationDomain = TaskDesc::GenerationDomain::Frame;

    std::atomic<int> unmetPrereqs{0};
    std::atomic<bool> submitted{false};
    std::atomic<bool> queued{false};
    std::atomic<bool> done{false};

    std::mutex successorMutex;
    std::vector<std::shared_ptr<TaskState>> successors;

    std::mutex doneMutex;
    std::condition_variable doneCv;
};

constexpr size_t kPriorityCount = static_cast<size_t>(Priority::Count);

struct DomainQueue
{
    std::mutex mutex;
    std::deque<std::shared_ptr<TaskState>> queue;
    std::atomic<uint32_t> pending{0};
    std::function<void()> wake;
};

struct Runtime
{
    std::vector<std::thread> workers;
    std::vector<std::thread> ioWorkers;
    std::atomic<bool> running{false};
    uint32_t workerCount = 0;
    uint32_t ioWorkerCount = 0;

    std::mutex queueMutex;
    std::condition_variable wakeCv;
    std::deque<std::shared_ptr<TaskState>> queues[kPriorityCount];

    DomainQueue render;
    DomainQueue logic;

    std::mutex ioMutex;
    std::condition_variable ioWakeCv;
    std::deque<std::shared_ptr<TaskState>> ioQueues[kPriorityCount];
    std::atomic<uint32_t> ioPending{0};

    std::mutex pipeMutex;
    std::unordered_map<std::string, std::unique_ptr<Pipe>> pipes;
    Pipe* loadSessionPipe = nullptr;

    std::atomic<uint64_t> frameGen{1};
    std::atomic<uint64_t> loadGen{1};
};

Runtime& runtime()
{
    static Runtime r;
    return r;
}

void completeTask(const std::shared_ptr<TaskState>& task);
void queueReadyTask(const std::shared_ptr<TaskState>& task);
void runTaskBody(const std::shared_ptr<TaskState>& task);

bool isStale(const TaskState& task)
{
    if (task.generation == 0)
        return false;
    const uint64_t current = (task.generationDomain == TaskDesc::GenerationDomain::Load)
        ? runtime().loadGen.load(std::memory_order_acquire)
        : runtime().frameGen.load(std::memory_order_acquire);
    return task.generation != current;
}

void runTaskBody(const std::shared_ptr<TaskState>& task)
{
    if (task->fn)
    {
        // Always invoke: fixed jobs own `user` lifetime and may need to publish
        // completion flags even when Load/Frame generation advanced.
        task->fn(task->user);
    }
    else if (task->body && !isStale(*task))
    {
        task->body();
    }

    if (task->pipe)
        task->pipe->onTaskFinished();

    completeTask(task);
}

void enqueueDomain(DomainQueue& domain, std::shared_ptr<TaskState> task)
{
    domain.pending.fetch_add(1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(domain.mutex);
        domain.queue.push_back(std::move(task));
    }
    if (domain.wake)
        domain.wake();
}

void enqueueIo(std::shared_ptr<TaskState> task)
{
    auto& rt = runtime();
    const size_t prio = std::min(static_cast<size_t>(task->priority), kPriorityCount - 1);
    rt.ioPending.fetch_add(1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(rt.ioMutex);
        rt.ioQueues[prio].push_back(std::move(task));
    }
    rt.ioWakeCv.notify_one();
}

void enqueueAny(std::shared_ptr<TaskState> task)
{
    auto& rt = runtime();
    const size_t prio = std::min(static_cast<size_t>(task->priority), kPriorityCount - 1);
    {
        std::lock_guard<std::mutex> lock(rt.queueMutex);
        rt.queues[prio].push_back(std::move(task));
    }
    rt.wakeCv.notify_one();
}

void dispatchByAffinity(std::shared_ptr<TaskState> task)
{
    switch (task->affinity)
    {
    case Affinity::Render:
        enqueueDomain(runtime().render, std::move(task));
        break;
    case Affinity::Logic:
        enqueueDomain(runtime().logic, std::move(task));
        break;
    case Affinity::IO:
        enqueueIo(std::move(task));
        break;
    case Affinity::Any:
    default:
        enqueueAny(std::move(task));
        break;
    }
}

void queueReadyTask(const std::shared_ptr<TaskState>& task)
{
    if (!task || task->done.load(std::memory_order_acquire))
        return;
    if (task->unmetPrereqs.load(std::memory_order_acquire) > 0)
        return;

    bool expected = false;
    if (!task->queued.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    if (task->pipe)
    {
        task->pipe->enqueue(task);
        return;
    }

    dispatchByAffinity(task);
}

void completeTask(const std::shared_ptr<TaskState>& task)
{
    {
        std::lock_guard<std::mutex> lock(task->doneMutex);
        task->done.store(true, std::memory_order_release);
    }
    task->doneCv.notify_all();

    std::vector<std::shared_ptr<TaskState>> successors;
    {
        std::lock_guard<std::mutex> lock(task->successorMutex);
        successors.swap(task->successors);
    }

    for (auto& succ : successors)
    {
        if (succ->unmetPrereqs.fetch_sub(1, std::memory_order_acq_rel) == 1
            && succ->submitted.load(std::memory_order_acquire))
        {
            queueReadyTask(succ);
        }
    }
}

bool helpOnceInternal()
{
    auto& rt = runtime();
    std::shared_ptr<TaskState> task;
    {
        std::lock_guard<std::mutex> lock(rt.queueMutex);
        for (size_t p = 0; p < kPriorityCount; ++p)
        {
            if (rt.queues[p].empty())
                continue;
            task = std::move(rt.queues[p].front());
            rt.queues[p].pop_front();
            break;
        }
    }
    if (!task)
        return false;
    runTaskBody(task);
    return true;
}

void pumpDomain(DomainQueue& domain)
{
    for (;;)
    {
        std::shared_ptr<TaskState> task;
        {
            std::lock_guard<std::mutex> lock(domain.mutex);
            if (domain.queue.empty())
                break;
            task = std::move(domain.queue.front());
            domain.queue.pop_front();
        }
        if (task)
        {
            runTaskBody(task);
            domain.pending.fetch_sub(1, std::memory_order_release);
        }
    }
}

uint32_t countQueued(const std::deque<std::shared_ptr<TaskState>>* queues, size_t n)
{
    uint32_t total = 0;
    for (size_t i = 0; i < n; ++i)
        total += static_cast<uint32_t>(queues[i].size());
    return total;
}

void workerMain()
{
    auto& rt = runtime();
    while (rt.running.load(std::memory_order_acquire))
    {
        std::shared_ptr<TaskState> task;
        {
            std::unique_lock<std::mutex> lock(rt.queueMutex);
            rt.wakeCv.wait_for(lock, std::chrono::milliseconds(2), [&] {
                if (!rt.running.load(std::memory_order_acquire))
                    return true;
                for (size_t p = 0; p < kPriorityCount; ++p)
                {
                    if (!rt.queues[p].empty())
                        return true;
                }
                return false;
            });

            if (!rt.running.load(std::memory_order_acquire))
                break;

            for (size_t p = 0; p < kPriorityCount; ++p)
            {
                if (rt.queues[p].empty())
                    continue;
                task = std::move(rt.queues[p].front());
                rt.queues[p].pop_front();
                break;
            }
        }

        if (task)
            runTaskBody(task);
    }
}

void ioWorkerMain()
{
    auto& rt = runtime();
    while (rt.running.load(std::memory_order_acquire))
    {
        std::shared_ptr<TaskState> task;
        {
            std::unique_lock<std::mutex> lock(rt.ioMutex);
            rt.ioWakeCv.wait_for(lock, std::chrono::milliseconds(2), [&] {
                if (!rt.running.load(std::memory_order_acquire))
                    return true;
                for (size_t p = 0; p < kPriorityCount; ++p)
                {
                    if (!rt.ioQueues[p].empty())
                        return true;
                }
                return false;
            });

            if (!rt.running.load(std::memory_order_acquire))
                break;

            for (size_t p = 0; p < kPriorityCount; ++p)
            {
                if (rt.ioQueues[p].empty())
                    continue;
                task = std::move(rt.ioQueues[p].front());
                rt.ioQueues[p].pop_front();
                break;
            }
        }

        if (task)
        {
            runTaskBody(task);
            rt.ioPending.fetch_sub(1, std::memory_order_release);
        }
    }
}

} // namespace detail

void Pipe::enqueue(std::shared_ptr<detail::TaskState> task)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.push_back(std::move(task));
    if (!m_running)
        scheduleNextUnlocked();
}

void Pipe::scheduleNextUnlocked()
{
    if (m_pending.empty())
    {
        m_running = false;
        return;
    }

    m_running = true;
    auto next = std::move(m_pending.front());
    m_pending.pop_front();
    detail::dispatchByAffinity(std::move(next));
}

void Pipe::onTaskFinished()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    scheduleNextUnlocked();
}

void initialize(uint32_t numWorkers, uint32_t reservedThreads)
{
    auto& rt = detail::runtime();
    if (rt.running.load(std::memory_order_acquire))
    {
        caustica::warning("task::initialize called more than once, ignoring.");
        return;
    }

    if (numWorkers == 0)
    {
        const unsigned hw = std::thread::hardware_concurrency();
        numWorkers = (hw > reservedThreads) ? (hw - reservedThreads) : 1;
    }
    numWorkers = std::max(1u, numWorkers);

    // One IO worker by default; keep at least one Any worker.
    const uint32_t ioWorkers = (numWorkers > 1) ? 1u : 1u;
    const uint32_t anyWorkers = std::max(1u, numWorkers);

    rt.workerCount = anyWorkers;
    rt.ioWorkerCount = ioWorkers;
    rt.running.store(true, std::memory_order_release);

    rt.loadSessionPipe = getPipe("LoadSession");

    rt.workers.clear();
    rt.workers.reserve(anyWorkers);
    for (uint32_t i = 0; i < anyWorkers; ++i)
        rt.workers.emplace_back(detail::workerMain);

    rt.ioWorkers.clear();
    rt.ioWorkers.reserve(ioWorkers);
    for (uint32_t i = 0; i < ioWorkers; ++i)
        rt.ioWorkers.emplace_back(detail::ioWorkerMain);

    caustica::info(
        "TaskRuntime initialized: %u Any workers, %u IO workers; pipe LoadSession",
        anyWorkers,
        ioWorkers);
}

void shutdown()
{
    auto& rt = detail::runtime();
    if (!rt.running.load(std::memory_order_acquire))
        return;

    rt.running.store(false, std::memory_order_release);
    rt.wakeCv.notify_all();
    rt.ioWakeCv.notify_all();

    for (auto& worker : rt.workers)
    {
        if (worker.joinable())
            worker.join();
    }
    rt.workers.clear();
    rt.workerCount = 0;

    for (auto& worker : rt.ioWorkers)
    {
        if (worker.joinable())
            worker.join();
    }
    rt.ioWorkers.clear();
    rt.ioWorkerCount = 0;

    {
        std::lock_guard<std::mutex> lock(rt.queueMutex);
        for (auto& q : rt.queues)
            q.clear();
    }
    {
        std::lock_guard<std::mutex> lock(rt.render.mutex);
        rt.render.queue.clear();
        rt.render.wake = nullptr;
        rt.render.pending.store(0, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(rt.logic.mutex);
        rt.logic.queue.clear();
        rt.logic.wake = nullptr;
        rt.logic.pending.store(0, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(rt.ioMutex);
        for (auto& q : rt.ioQueues)
            q.clear();
        rt.ioPending.store(0, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> lock(rt.pipeMutex);
        rt.pipes.clear();
        rt.loadSessionPipe = nullptr;
    }
}

bool isInitialized()
{
    return detail::runtime().running.load(std::memory_order_acquire);
}

uint32_t workerCount()
{
    return detail::runtime().workerCount;
}

Pipe* getPipe(const char* name)
{
    if (!name || !*name)
        name = "unnamed";

    auto& rt = detail::runtime();
    std::lock_guard<std::mutex> lock(rt.pipeMutex);
    auto it = rt.pipes.find(name);
    if (it != rt.pipes.end())
        return it->second.get();

    auto pipe = std::make_unique<Pipe>(name);
    Pipe* raw = pipe.get();
    rt.pipes.emplace(name, std::move(pipe));
    return raw;
}

Pipe* loadSessionPipe()
{
    return detail::runtime().loadSessionPipe;
}

TaskHandle create(TaskDesc desc)
{
    auto state = std::make_shared<detail::TaskState>();
    state->fn = desc.fn;
    state->user = desc.user;
    state->body = std::move(desc.body);
    state->name = desc.name;
    state->priority = desc.priority;
    state->affinity = desc.affinity;
    state->pipe = desc.pipe;
    state->generation = desc.generation;
    state->generationDomain = desc.generationDomain;

    TaskHandle handle;
    handle.m_state = std::static_pointer_cast<void>(std::move(state));
    return handle;
}

void submit(TaskHandle handle)
{
    auto state = std::static_pointer_cast<detail::TaskState>(handle.m_state);
    if (!state)
        return;

    state->submitted.store(true, std::memory_order_release);
    if (state->unmetPrereqs.load(std::memory_order_acquire) == 0)
        detail::queueReadyTask(state);
}

TaskHandle launch(TaskDesc desc)
{
    TaskHandle handle = create(std::move(desc));
    submit(handle);
    return handle;
}

TaskHandle launch(
    const char* name,
    Priority priority,
    Affinity affinity,
    TaskFn fn,
    void* user,
    Pipe* pipe,
    uint64_t generation,
    TaskDesc::GenerationDomain generationDomain)
{
    TaskDesc desc;
    desc.name = name;
    desc.priority = priority;
    desc.affinity = affinity;
    desc.fn = fn;
    desc.user = user;
    desc.pipe = pipe;
    desc.generation = generation;
    desc.generationDomain = generationDomain;
    return launch(std::move(desc));
}

void then(TaskHandle prerequisite, TaskHandle subsequent)
{
    auto pre = std::static_pointer_cast<detail::TaskState>(prerequisite.m_state);
    auto post = std::static_pointer_cast<detail::TaskState>(subsequent.m_state);
    if (!pre || !post || pre == post)
        return;

    if (pre->done.load(std::memory_order_acquire))
        return;

    post->unmetPrereqs.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(pre->successorMutex);
        if (!pre->done.load(std::memory_order_acquire))
        {
            pre->successors.push_back(post);
            return;
        }
    }
    post->unmetPrereqs.fetch_sub(1, std::memory_order_acq_rel);
}

void wait(TaskHandle handle)
{
    auto state = std::static_pointer_cast<detail::TaskState>(handle.m_state);
    if (!state)
        return;

    while (!state->done.load(std::memory_order_acquire))
    {
        if (!detail::helpOnceInternal())
        {
            std::unique_lock<std::mutex> lock(state->doneMutex);
            state->doneCv.wait_for(lock, std::chrono::milliseconds(1), [&] {
                return state->done.load(std::memory_order_acquire);
            });
        }
    }
}

bool poll(TaskHandle handle)
{
    auto state = std::static_pointer_cast<detail::TaskState>(handle.m_state);
    return state && state->done.load(std::memory_order_acquire);
}

void helpOnce()
{
    detail::helpOnceInternal();
}

void setRenderWake(std::function<void()> wake)
{
    auto& rt = detail::runtime();
    std::lock_guard<std::mutex> lock(rt.render.mutex);
    rt.render.wake = std::move(wake);
}

void pumpRender()
{
    detail::pumpDomain(detail::runtime().render);
}

bool isRenderDomainBusy()
{
    return detail::runtime().render.pending.load(std::memory_order_acquire) > 0;
}

void pumpLogic()
{
    detail::pumpDomain(detail::runtime().logic);
}

bool isLogicDomainBusy()
{
    return detail::runtime().logic.pending.load(std::memory_order_acquire) > 0;
}

bool isIoDomainBusy()
{
    return detail::runtime().ioPending.load(std::memory_order_acquire) > 0;
}

RuntimeStats snapshotStats()
{
    auto& rt = detail::runtime();
    RuntimeStats stats;
    stats.workerCount = rt.workerCount;
    stats.ioWorkerCount = rt.ioWorkerCount;
    stats.frameGeneration = rt.frameGen.load(std::memory_order_acquire);
    stats.loadGeneration = rt.loadGen.load(std::memory_order_acquire);
    stats.renderQueued = rt.render.pending.load(std::memory_order_acquire);
    stats.logicQueued = rt.logic.pending.load(std::memory_order_acquire);
    stats.ioQueued = rt.ioPending.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> lock(rt.queueMutex);
        stats.anyQueued = detail::countQueued(rt.queues, detail::kPriorityCount);
    }
    return stats;
}

uint64_t frameGeneration()
{
    return detail::runtime().frameGen.load(std::memory_order_acquire);
}

void bumpFrameGeneration()
{
    detail::runtime().frameGen.fetch_add(1, std::memory_order_acq_rel);
}

uint64_t loadGeneration()
{
    return detail::runtime().loadGen.load(std::memory_order_acquire);
}

void bumpLoadGeneration()
{
    detail::runtime().loadGen.fetch_add(1, std::memory_order_acq_rel);
}

void launch(Group& group, TaskDesc desc)
{
    group.pending.fetch_add(1, std::memory_order_release);

    // Wrap completion for both fixed-fn and std::function bodies.
    if (desc.fn)
    {
        TaskFn fn = desc.fn;
        void* user = desc.user;
        desc.fn = nullptr;
        desc.user = nullptr;
        desc.body = [&group, fn, user]() {
            if (fn)
                fn(user);
            group.pending.fetch_sub(1, std::memory_order_release);
        };
    }
    else
    {
        auto body = std::move(desc.body);
        desc.body = [&group, body = std::move(body)]() {
            if (body)
                body();
            group.pending.fetch_sub(1, std::memory_order_release);
        };
    }

    if (!isInitialized())
    {
        if (desc.body)
            desc.body();
        return;
    }

    (void)launch(std::move(desc));
}

void wait(Group& group)
{
    while (group.pending.load(std::memory_order_acquire) > 0)
    {
        if (!detail::helpOnceInternal())
            std::this_thread::yield();
    }
}

bool isBusy(const Group& group)
{
    return group.pending.load(std::memory_order_acquire) > 0;
}

void parallelFor(Group& group,
                 uint32_t count,
                 Priority priority,
                 Affinity affinity,
                 std::function<void(uint32_t index)> fn,
                 uint32_t groupSize)
{
    if (count == 0 || !fn)
        return;

    if (groupSize == 0)
        groupSize = 1;

    if (!isInitialized())
    {
        for (uint32_t i = 0; i < count; ++i)
            fn(i);
        return;
    }

    const uint32_t groups = (count + groupSize - 1) / groupSize;
    for (uint32_t g = 0; g < groups; ++g)
    {
        const uint32_t begin = g * groupSize;
        const uint32_t end = std::min(begin + groupSize, count);
        TaskDesc desc;
        desc.name = "parallelFor";
        desc.priority = priority;
        desc.affinity = affinity;
        desc.body = [fn, begin, end]() {
            for (uint32_t i = begin; i < end; ++i)
                fn(i);
        };
        launch(group, std::move(desc));
    }
}

} // namespace caustica::task

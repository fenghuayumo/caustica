#include <core/JobSystem.h>
#include <core/log.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace caustica
{
namespace JobSystem
{

// ==========================================================================
// Internal job representation
// ==========================================================================
struct alignas(64) Job
{
    std::function<void()> func;
};

// ==========================================================================
// Per-worker state (cache-line padded to reduce false sharing)
// ==========================================================================
struct alignas(64) WorkerState
{
    std::thread       thread;
    std::deque<Job>   localQueue;
    std::mutex        queueMutex;
    std::atomic<bool> wakeFlag{false};
    std::atomic<bool> running{true};
};

// ==========================================================================
// Global state
// ==========================================================================
static std::vector<std::unique_ptr<WorkerState>> s_Workers;
static uint32_t                 s_NumThreads = 0;
static bool                     s_Initialized = false;
static std::atomic<uint32_t>    s_NextWorker{0};

// ==========================================================================
// Forward notifications to idle workers
// ==========================================================================
namespace
{
    std::condition_variable s_WakeCondition;
    std::mutex              s_WakeMutex;
    std::atomic<uint32_t>   s_IdleCount{0};

    void NotifyOne()
    {
        s_WakeCondition.notify_one();
    }

    void NotifyAll()
    {
        s_WakeCondition.notify_all();
    }
} // anonymous namespace

// ==========================================================================
// Initialize
// ==========================================================================
void Initialize(uint32_t numThreads, uint32_t reservedThreads)
{
    if (s_Initialized)
    {
        caustica::warning("JobSystem::Initialize called more than once, ignoring.");
        return;
    }

    if (numThreads == 0)
    {
        unsigned int hw = std::thread::hardware_concurrency();
        numThreads = (hw > reservedThreads) ? (hw - reservedThreads) : 1;
    }
    numThreads = std::max(1u, numThreads);

    s_NumThreads = numThreads;
    s_Workers.clear();
    s_Workers.reserve(numThreads);

    for (uint32_t i = 0; i < numThreads; ++i)
        s_Workers.push_back(std::make_unique<WorkerState>());

    for (uint32_t i = 0; i < numThreads; ++i)
    {
        WorkerState* worker = s_Workers[i].get();
        worker->thread = std::thread([worker, i]()
        {
            while (worker->running.load(std::memory_order_acquire))
            {
                // Try to get work from the global pool or steal
                bool didWork = false;

                // process local queue
                Job localJob;
                {
                    std::lock_guard<std::mutex> lock(worker->queueMutex);
                    if (!worker->localQueue.empty())
                    {
                        localJob = std::move(worker->localQueue.front());
                        worker->localQueue.pop_front();
                        didWork = true;
                    }
                }
                if (localJob.func)
                {
                    localJob.func();
                    continue;
                }

                // Try work-stealing from other workers
                if (!didWork)
                {
                    for (uint32_t j = 0; j < s_NumThreads; ++j)
                    {
                        uint32_t victimIdx = (i + j + 1) % s_NumThreads;
                        WorkerState& victim = *s_Workers[victimIdx];
                        if (&victim == worker) continue;

                        Job stolenJob;
                        {
                            std::lock_guard<std::mutex> lock(victim.queueMutex);
                            if (!victim.localQueue.empty())
                            {
                                stolenJob = std::move(victim.localQueue.back());
                                victim.localQueue.pop_back();
                                didWork = true;
                            }
                        }
                        if (stolenJob.func)
                        {
                            stolenJob.func();
                            break;
                        }
                    }
                }

                if (!didWork)
                {
                    // Sleep until woken with new work
                    s_IdleCount.fetch_add(1, std::memory_order_relaxed);
                    std::unique_lock<std::mutex> wakeLock(s_WakeMutex);
                    s_WakeCondition.wait_for(wakeLock, std::chrono::milliseconds(1));
                    s_IdleCount.fetch_sub(1, std::memory_order_relaxed);
                }
            }
        });
    }

    s_Initialized = true;
    caustica::info("JobSystem initialized with %u worker threads", numThreads);
}

// ==========================================================================
// shutdown
// ==========================================================================
void shutdown()
{
    if (!s_Initialized)
        return;

    for (auto& worker : s_Workers)
        worker->running.store(false, std::memory_order_release);

    NotifyAll();

    for (auto& worker : s_Workers)
    {
        if (worker->thread.joinable())
            worker->thread.join();
    }

    s_Workers.clear();
    s_NumThreads = 0;
    s_Initialized = false;
}

// ==========================================================================
// getThreadCount
// ==========================================================================
uint32_t getThreadCount()
{
    return s_NumThreads;
}

// ==========================================================================
// execute — submit a single job
// ==========================================================================
void execute(Context& ctx, std::function<void()> task)
{
    if (!s_Initialized || s_NumThreads == 0)
    {
        task();
        return;
    }

    ctx.counter.fetch_add(1, std::memory_order_release);

    // Pick a worker round-robin
    uint32_t idx = s_NextWorker.fetch_add(1, std::memory_order_relaxed) % s_NumThreads;
    WorkerState& worker = *s_Workers[idx];

    // Wrap task to auto-decrement counter on completion
    auto wrapped = [task = std::move(task), &ctx]()
    {
        task();
        ctx.counter.fetch_sub(1, std::memory_order_release);
    };

    {
        std::lock_guard<std::mutex> lock(worker.queueMutex);
        worker.localQueue.push_back(Job{std::move(wrapped)});
    }

    NotifyOne();
}

// ==========================================================================
// dispatch — parallel-for
// ==========================================================================
void dispatch(Context& ctx, uint32_t jobCount, uint32_t groupSize,
              std::function<void(JobDispatchArgs)> task)
{
    if (jobCount == 0)
        return;

    if (!s_Initialized || s_NumThreads == 0)
    {
        for (uint32_t i = 0; i < jobCount; ++i)
        {
            JobDispatchArgs args;
            args.jobIndex = i;
            args.groupID = 0;
            args.groupIndex = i;
            args.isFirstJobInGroup = (i == 0);
            args.isLastJobInGroup = (i == jobCount - 1);
            task(args);
        }
        return;
    }

    uint32_t groupCount = getGroupCount(jobCount, groupSize);
    ctx.counter.fetch_add(groupCount, std::memory_order_release);

    // Distribute groups across workers
    for (uint32_t groupID = 0; groupID < groupCount; ++groupID)
    {
        uint32_t idx = s_NextWorker.fetch_add(1, std::memory_order_relaxed) % s_NumThreads;
        WorkerState& worker = *s_Workers[idx];

        uint32_t groupStart = groupID * groupSize;
        uint32_t groupEnd = std::min(groupStart + groupSize, jobCount);

        auto groupTask = [task, groupStart, groupEnd, groupID, &ctx]()
        {
            for (uint32_t i = groupStart; i < groupEnd; ++i)
            {
                JobDispatchArgs args;
                args.jobIndex = i;
                args.groupID = groupID;
                args.groupIndex = i - groupStart;
                args.isFirstJobInGroup = (i == groupStart);
                args.isLastJobInGroup = (i == groupEnd - 1);
                task(args);
            }
            ctx.counter.fetch_sub(1, std::memory_order_release);
        };

        {
            std::lock_guard<std::mutex> lock(worker.queueMutex);
            worker.localQueue.push_back(Job{groupTask});
        }
    }

    NotifyAll();
}

// ==========================================================================
// getGroupCount
// ==========================================================================
uint32_t getGroupCount(uint32_t jobCount, uint32_t groupSize)
{
    return (jobCount + groupSize - 1) / groupSize;
}

// ==========================================================================
// isBusy — check if any jobs in this context are running
// ==========================================================================
bool isBusy(const Context& ctx)
{
    return ctx.counter.load(std::memory_order_acquire) > 0;
}

// ==========================================================================
// wait — block until all jobs in context complete
// ==========================================================================
void wait(const Context& ctx)
{
    // Spin briefly, then yield
    while (ctx.counter.load(std::memory_order_acquire) > 0)
    {
        // Help out: process local work while waiting
        std::this_thread::yield();
    }
}

} // namespace JobSystem
} // namespace caustica

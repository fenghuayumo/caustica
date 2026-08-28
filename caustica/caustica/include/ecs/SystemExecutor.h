#pragma once

#include <core/task/TaskRuntime.h>
#include <ecs/SystemAccess.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace caustica::ecs
{

// One system in a prepared schedule. Edges already encode both the explicit
// ordering constraints (runBefore / runAfter / sets) and the implicit ones the
// planner inserts between systems whose access conflicts, so the executor never
// has to test access at runtime.
struct SystemNode
{
    SystemAccess access;
    std::vector<int> dependents;
    int prerequisiteCount = 0;
};

struct SystemExecutorStats
{
    uint32_t systemsRun = 0;
    // Systems handed to TaskRuntime workers.
    uint32_t dispatchedSystems = 0;
    // Systems run on the scheduling thread: every exclusive one, plus the first
    // of each dispatch round so this thread does not just block.
    uint32_t inlineSystems = 0;
    uint32_t peakConcurrency = 0;
};

// Runs a prepared schedule with as much overlap as the graph allows.
//
// `order` is a topological order of `nodes`; it also fixes dispatch priority so
// a schedule always starts its systems in the same sequence. `run` is invoked
// once per system index and must be safe to call concurrently for non-exclusive
// nodes. Exclusive nodes are always invoked on the calling thread with nothing
// else in flight.
inline void runSystemsParallel(
    const std::vector<SystemNode>& nodes,
    const std::vector<int>& order,
    const std::function<void(int)>& run,
    SystemExecutorStats* stats = nullptr)
{
    const std::size_t count = nodes.size();
    if (count == 0 || !run)
        return;

    SystemExecutorStats localStats;

    if (!task::isInitialized() || task::workerCount() == 0)
    {
        for (int index : order)
            run(index);
        localStats.systemsRun = static_cast<uint32_t>(count);
        localStats.inlineSystems = static_cast<uint32_t>(count);
        localStats.peakConcurrency = 1;
        if (stats)
            *stats = localStats;
        return;
    }

    struct SharedState
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::vector<int> completed;
        std::atomic<uint32_t> inFlight{ 0 };
    };
    SharedState shared;

    std::vector<int> remaining(count);
    std::vector<uint8_t> started(count, 0);
    for (std::size_t i = 0; i < count; ++i)
        remaining[i] = nodes[i].prerequisiteCount;

    std::size_t finished = 0;
    std::vector<int> readyBatch;
    std::vector<int> completedBatch;

    auto retire = [&](int index) {
        ++finished;
        for (int dependent : nodes[static_cast<std::size_t>(index)].dependents)
            --remaining[static_cast<std::size_t>(dependent)];
    };

    auto notePeak = [&](uint32_t concurrency) {
        if (concurrency > localStats.peakConcurrency)
            localStats.peakConcurrency = concurrency;
    };

    while (finished < count)
    {
        readyBatch.clear();
        for (int index : order)
        {
            const auto slot = static_cast<std::size_t>(index);
            if (started[slot] || remaining[slot] != 0)
                continue;

            if (nodes[slot].access.exclusive)
            {
                // An exclusive system is ordered against every other system in
                // the phase, so reaching it normally means nothing else is
                // pending. The guard only matters for a malformed plan.
                if (!readyBatch.empty() || shared.inFlight.load(std::memory_order_acquire) != 0)
                    break;

                started[slot] = 1;
                run(index);
                ++localStats.systemsRun;
                ++localStats.inlineSystems;
                notePeak(1);
                retire(index);
                continue;
            }

            started[slot] = 1;
            readyBatch.push_back(index);
        }

        if (!readyBatch.empty())
        {
            // The first system stays on this thread. Blocking here instead would
            // waste a core, and draining unrelated Any-affinity work risks
            // pulling a long background job into the middle of the frame.
            const int inlineIndex = readyBatch.front();
            const auto dispatchCount = static_cast<uint32_t>(readyBatch.size() - 1);
            notePeak(dispatchCount + 1);

            if (dispatchCount > 0)
            {
                shared.inFlight.fetch_add(dispatchCount, std::memory_order_release);
                for (std::size_t i = 1; i < readyBatch.size(); ++i)
                {
                    const int index = readyBatch[i];
                    task::TaskDesc desc;
                    desc.name = "Schedule.System";
                    desc.priority = task::Priority::High;
                    desc.affinity = task::Affinity::Any;
                    desc.body = [&shared, &run, index]() {
                        run(index);
                        {
                            std::lock_guard<std::mutex> lock(shared.mutex);
                            shared.completed.push_back(index);
                        }
                        shared.inFlight.fetch_sub(1, std::memory_order_acq_rel);
                        shared.cv.notify_one();
                    };
                    (void)task::launch(std::move(desc));
                }
                localStats.dispatchedSystems += dispatchCount;
            }

            run(inlineIndex);
            ++localStats.inlineSystems;
            localStats.systemsRun += static_cast<uint32_t>(readyBatch.size());
            retire(inlineIndex);
            continue;
        }

        {
            std::unique_lock<std::mutex> lock(shared.mutex);
            if (shared.completed.empty())
            {
                if (shared.inFlight.load(std::memory_order_acquire) == 0)
                {
                    lock.unlock();
                    // Nothing ready and nothing running. Either an exclusive
                    // system just became eligible, or the plan has a cycle.
                    bool anyEligible = false;
                    for (int index : order)
                    {
                        const auto slot = static_cast<std::size_t>(index);
                        if (!started[slot] && remaining[slot] == 0)
                        {
                            anyEligible = true;
                            break;
                        }
                    }
                    if (anyEligible)
                        continue;

                    for (int index : order)
                    {
                        const auto slot = static_cast<std::size_t>(index);
                        if (started[slot])
                            continue;
                        started[slot] = 1;
                        run(index);
                        ++localStats.systemsRun;
                        ++localStats.inlineSystems;
                        retire(index);
                    }
                    break;
                }
                shared.cv.wait(lock, [&shared] { return !shared.completed.empty(); });
            }
            completedBatch.clear();
            completedBatch.swap(shared.completed);
        }

        for (int index : completedBatch)
            retire(index);
    }

    if (stats)
        *stats = localStats;
}

} // namespace caustica::ecs

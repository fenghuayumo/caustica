#include <core/task/TaskRuntime.h>

#include <atomic>
#include <cstdio>
#include <utility>
#include <vector>

namespace
{
bool expect(bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "TaskRuntime test failed: %s\n", message);
    return false;
}
}

int main()
{
    using namespace caustica::task;

    initialize(2);
    bool passed = true;

    std::vector<int> domainOrder;
    TaskDesc low;
    low.name = "Logic.Low";
    low.priority = Priority::Low;
    low.affinity = Affinity::Logic;
    low.body = [&] { domainOrder.push_back(1); };
    TaskHandle lowHandle = launch(std::move(low));

    TaskDesc critical;
    critical.name = "Logic.Critical";
    critical.priority = Priority::Critical;
    critical.affinity = Affinity::Logic;
    critical.body = [&] { domainOrder.push_back(2); };
    TaskHandle criticalHandle = launch(std::move(critical));

    pumpLogic();
    passed &= expect(poll(lowHandle) && poll(criticalHandle), "Logic pump did not complete queued work");
    passed &= expect(
        domainOrder == std::vector<int>({ 2, 1 }),
        "Logic domain did not honor task priority");

    std::atomic<uint32_t> staleExecutions{0};
    Group staleGroup;
    TaskDesc stale;
    stale.name = "Stale.Group";
    stale.affinity = Affinity::Any;
    stale.generation = frameGeneration();
    stale.body = [&] { staleExecutions.fetch_add(1, std::memory_order_relaxed); };
    bumpFrameGeneration();
    launch(staleGroup, std::move(stale));
    wait(staleGroup);
    passed &= expect(staleExecutions.load(std::memory_order_relaxed) == 0, "Stale task body executed");
    passed &= expect(!isBusy(staleGroup), "Stale task did not complete its Group");

    std::atomic<uint32_t> liveExecutions{0};
    Group liveGroup;
    TaskDesc live;
    live.name = "Live.Group";
    live.affinity = Affinity::Any;
    live.body = [&] { liveExecutions.fetch_add(1, std::memory_order_relaxed); };
    launch(liveGroup, std::move(live));
    wait(liveGroup);
    passed &= expect(liveExecutions.load(std::memory_order_relaxed) == 1, "Live Group task did not execute once");

    std::atomic<uint32_t> parallelExecutions{0};
    std::atomic<uint32_t> parallelSum{0};
    Group parallelGroup;
    parallelFor(
        parallelGroup,
        64,
        Priority::High,
        Affinity::Any,
        [&](uint32_t index) {
            parallelExecutions.fetch_add(1, std::memory_order_relaxed);
            parallelSum.fetch_add(index, std::memory_order_relaxed);
        },
        4);
    wait(parallelGroup);
    passed &= expect(parallelExecutions.load(std::memory_order_relaxed) == 64,
        "Batched parallelFor did not execute every index once");
    passed &= expect(parallelSum.load(std::memory_order_relaxed) == (63u * 64u) / 2u,
        "Batched parallelFor produced an invalid index range");
    passed &= expect(!isBusy(parallelGroup), "Batched parallelFor did not complete its Group");

    std::atomic<bool> ioCompleted{false};
    TaskDesc io;
    io.name = "IO.AtomicWait";
    io.affinity = Affinity::IO;
    io.body = [&] { ioCompleted.store(true, std::memory_order_release); };
    TaskHandle ioHandle = launch(std::move(io));
    wait(ioHandle);
    passed &= expect(ioCompleted.load(std::memory_order_acquire), "TaskHandle atomic wait missed IO completion");

    shutdown();
    return passed ? 0 : 1;
}

#include <core/task/TaskRuntime.h>
#include <ecs/SystemAccess.h>
#include <ecs/SystemExecutor.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
bool expect(bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "SystemScheduler test failed: %s\n", message);
    return false;
}

struct Position
{
    float x = 0.0f;
};

struct Velocity
{
    float x = 0.0f;
};

struct Health
{
    int value = 0;
};

struct FrameClock
{
    float delta = 0.0f;
};

struct Physics
{
    int steps = 0;
};

using caustica::ecs::SystemAccess;
using caustica::ecs::SystemNode;

// Turns a plain successor list into the prerequisite counts the executor needs.
std::vector<SystemNode> makeNodes(
    std::vector<SystemAccess> accesses,
    const std::vector<std::pair<int, int>>& edges)
{
    std::vector<SystemNode> nodes(accesses.size());
    for (std::size_t i = 0; i < accesses.size(); ++i)
        nodes[i].access = std::move(accesses[i]);
    for (const auto& [from, to] : edges)
    {
        nodes[static_cast<std::size_t>(from)].dependents.push_back(to);
        ++nodes[static_cast<std::size_t>(to)].prerequisiteCount;
    }
    return nodes;
}

std::vector<int> identityOrder(std::size_t count)
{
    std::vector<int> order(count);
    for (std::size_t i = 0; i < count; ++i)
        order[i] = static_cast<int>(i);
    return order;
}
} // namespace

int main()
{
    using namespace caustica::ecs;

    caustica::task::initialize(4);
    bool passed = true;

    // Dense type ids are stable and distinct.
    {
        passed &= expect(typeId<Position>() == typeId<Position>(), "type id was not stable");
        passed &= expect(typeId<Position>() != typeId<Velocity>(), "distinct types shared a type id");
        passed &= expect(typeId<Position>() == typeId<const Position>(),
            "const-qualified type produced a different id");
    }

    // AccessMask set / intersect.
    {
        AccessMask a;
        AccessMask b;
        passed &= expect(a.empty(), "fresh mask was not empty");
        passed &= expect(!a.intersects(b), "empty masks reported an intersection");

        a.set(typeId<Position>());
        b.set(typeId<Velocity>());
        passed &= expect(!a.intersects(b), "disjoint masks reported an intersection");

        b.set(typeId<Position>());
        passed &= expect(a.intersects(b), "overlapping masks reported no intersection");
        passed &= expect(a.test(typeId<Position>()), "mask lost a bit it was given");
        passed &= expect(!a.test(typeId<Velocity>()), "mask gained a bit it was never given");
    }

    // Conflict rules: shared reads are fine, a write against anything is not.
    {
        SystemAccess readPosition = SystemAccess::makeParallel();
        readPosition.readComponent<Position>();

        SystemAccess alsoReadPosition = SystemAccess::makeParallel();
        alsoReadPosition.readComponent<Position>();

        SystemAccess writePosition = SystemAccess::makeParallel();
        writePosition.writeComponent<Position>();

        SystemAccess writeVelocity = SystemAccess::makeParallel();
        writeVelocity.writeComponent<Velocity>();

        SystemAccess exclusive;

        passed &= expect(!readPosition.conflictsWith(alsoReadPosition),
            "two readers of the same component were treated as conflicting");
        passed &= expect(readPosition.conflictsWith(writePosition),
            "a reader and a writer of the same component were treated as compatible");
        passed &= expect(writePosition.conflictsWith(readPosition),
            "conflict detection was not symmetric");
        passed &= expect(!writePosition.conflictsWith(writeVelocity),
            "writers of different components were treated as conflicting");
        passed &= expect(exclusive.conflictsWith(readPosition),
            "an exclusive system was allowed to overlap");
        passed &= expect(readPosition.conflictsWith(exclusive),
            "an exclusive system was allowed to overlap from the other side");
    }

    // Resource access is tracked separately from components.
    {
        SystemAccess readClock = SystemAccess::makeParallel();
        readClock.readResource<FrameClock>();

        SystemAccess writeClock = SystemAccess::makeParallel();
        writeClock.writeResource<FrameClock>();

        SystemAccess writePhysics = SystemAccess::makeParallel();
        writePhysics.writeResource<Physics>();

        passed &= expect(readClock.conflictsWith(writeClock),
            "resource read/write overlap was missed");
        passed &= expect(!writeClock.conflictsWith(writePhysics),
            "writers of different resources were treated as conflicting");

        SystemAccess componentOnly = SystemAccess::makeParallel();
        componentOnly.writeComponent<Health>();
        passed &= expect(!componentOnly.conflictsWith(writeClock),
            "a component write conflicted with an unrelated resource write");
    }

    // Writing a component records a change-tick warm-up so workers never insert
    // into the registry context concurrently.
    {
        SystemAccess access = SystemAccess::makeParallel();
        access.writeComponent<Position>();
        access.writeComponent<Position>();
        passed &= expect(access.componentWarmups.size() == 1,
            "repeated writes registered duplicate warm-ups");

        World world;
        world.enableChangeDetection();
        access.applyWarmups(world);
        const Entity entity = world.spawn();
        world.emplace<Position>(entity);
        world.notifyComponentChanged<Position>(entity);

        auto* changeDetection = world.getResource<ChangeDetection>();
        passed &= expect(changeDetection != nullptr, "change detection resource went missing");
        passed &= expect(changeDetection && changeDetection->isChangedThisFrame<Position>(entity, world.registry()),
            "warm-up path broke change detection");
    }

    // Executor: dependencies are honoured and every node runs exactly once.
    {
        std::mutex mutex;
        std::vector<int> log;
        std::vector<int> runCount(4, 0);

        std::vector<SystemAccess> accesses(4, SystemAccess::makeParallel());
        // 0 -> 1 -> 3, 0 -> 2 -> 3
        std::vector<SystemNode> nodes = makeNodes(accesses, { { 0, 1 }, { 0, 2 }, { 1, 3 }, { 2, 3 } });

        SystemExecutorStats stats;
        runSystemsParallel(nodes, identityOrder(nodes.size()), [&](int index) {
            std::lock_guard<std::mutex> lock(mutex);
            log.push_back(index);
            ++runCount[static_cast<std::size_t>(index)];
        }, &stats);

        passed &= expect(log.size() == 4, "executor did not run every system");
        for (int count : runCount)
            passed &= expect(count == 1, "a system ran more than once");
        passed &= expect(!log.empty() && log.front() == 0, "the root system did not run first");
        passed &= expect(!log.empty() && log.back() == 3, "the join system did not run last");
        passed &= expect(stats.systemsRun == 4, "executor stats undercounted systems");
    }

    // Executor: independent systems overlap.
    {
        constexpr int kSystemCount = 4;
        std::atomic<int> concurrent{ 0 };
        std::atomic<int> peak{ 0 };
        std::atomic<int> completed{ 0 };

        std::vector<SystemAccess> accesses;
        accesses.reserve(kSystemCount);
        accesses.push_back(SystemAccess::makeParallel());
        accesses.back().writeComponent<Position>();
        accesses.push_back(SystemAccess::makeParallel());
        accesses.back().writeComponent<Velocity>();
        accesses.push_back(SystemAccess::makeParallel());
        accesses.back().writeComponent<Health>();
        accesses.push_back(SystemAccess::makeParallel());
        accesses.back().writeResource<Physics>();

        std::vector<SystemNode> nodes = makeNodes(accesses, {});

        runSystemsParallel(nodes, identityOrder(nodes.size()), [&](int) {
            const int now = concurrent.fetch_add(1) + 1;
            int observed = peak.load();
            while (now > observed && !peak.compare_exchange_weak(observed, now))
            {
            }
            // Hold the system open until someone else joins, so the overlap is
            // observable without relying on a sleep. Bounded so a machine that
            // really cannot overlap still finishes.
            for (int i = 0; i < 200000 && peak.load() < 2; ++i)
                std::this_thread::yield();
            concurrent.fetch_sub(1);
            completed.fetch_add(1);
        });

        passed &= expect(completed.load() == kSystemCount, "not every independent system ran");
        passed &= expect(peak.load() > 1, "independent systems never overlapped");
    }

    // Executor: an exclusive system runs alone.
    {
        std::atomic<int> concurrent{ 0 };
        std::atomic<bool> exclusiveSawCompany{ false };
        std::atomic<int> completed{ 0 };

        // Distinct access on the outer systems, so only the exclusive middle one
        // forces any ordering.
        std::vector<SystemAccess> accesses(3, SystemAccess::makeParallel());
        accesses[0].writeComponent<Position>();
        accesses[1] = SystemAccess{};
        accesses[2].writeComponent<Velocity>();

        // Exclusive systems are ordered against everything by the planner.
        std::vector<SystemNode> nodes = makeNodes(accesses, { { 0, 1 }, { 1, 2 } });

        SystemExecutorStats stats;
        runSystemsParallel(nodes, identityOrder(nodes.size()), [&](int index) {
            const int now = concurrent.fetch_add(1) + 1;
            if (index == 1 && now != 1)
                exclusiveSawCompany.store(true);
            for (volatile int i = 0; i < 50000; ++i)
            {
            }
            concurrent.fetch_sub(1);
            completed.fetch_add(1);
        }, &stats);

        passed &= expect(completed.load() == 3, "not every system ran around the exclusive one");
        passed &= expect(!exclusiveSawCompany.load(), "an exclusive system ran beside another system");
        passed &= expect(stats.inlineSystems >= 1, "the exclusive system was not run inline");
    }

    // Executor falls back to serial when the runtime is unavailable.
    {
        caustica::task::shutdown();

        std::vector<int> log;
        std::vector<SystemAccess> accesses(3, SystemAccess::makeParallel());
        std::vector<SystemNode> nodes = makeNodes(accesses, {});
        SystemExecutorStats stats;
        runSystemsParallel(nodes, identityOrder(nodes.size()), [&](int index) {
            log.push_back(index);
        }, &stats);

        passed &= expect(log.size() == 3 && log[0] == 0 && log[1] == 1 && log[2] == 2,
            "serial fallback did not run systems in plan order");
        passed &= expect(stats.dispatchedSystems == 0, "serial fallback dispatched to workers");
    }

    return passed ? 0 : 1;
}

#include <engine/AppSchedules.h>
#include <engine/SceneQuery.h>

#include <algorithm>
#include <cstdio>
#include <utility>

namespace caustica
{

scene::SceneEntityWorld* SystemContext::entityWorld()
{
    return caustica::entityWorld(app);
}

const scene::SceneEntityWorld* SystemContext::entityWorld() const
{
    return caustica::entityWorld(app);
}

ecs::World* SystemContext::sceneEcs()
{
    return caustica::sceneEcs(app);
}

const ecs::World* SystemContext::sceneEcs() const
{
    return caustica::sceneEcs(app);
}


const char* toString(AppSchedule schedule)
{
    switch (schedule)
    {
    case AppSchedule::Startup: return "Startup";
    case AppSchedule::First: return "First";
    case AppSchedule::preUpdate: return "preUpdate";
    case AppSchedule::update: return "update";
    case AppSchedule::PostUpdate: return "PostUpdate";
    case AppSchedule::Extract: return "Extract";
    case AppSchedule::render: return "render";
    case AppSchedule::postRender: return "postRender";
    case AppSchedule::Last: return "Last";
    case AppSchedule::shutdown: return "shutdown";
    default: return "Unknown";
    }
}

namespace
{

// --- Reachability bitsets --------------------------------------------------
// The planner needs to know whether two systems are already ordered before it
// inserts an implicit edge between them, otherwise a conflicting pair could
// close a cycle against an explicit runAfter.

constexpr std::size_t kBitsPerWord = 64;

std::size_t wordCount(std::size_t bits)
{
    return (bits + kBitsPerWord - 1) / kBitsPerWord;
}

bool testBit(const std::vector<uint64_t>& bits, std::size_t index)
{
    return (bits[index / kBitsPerWord] & (uint64_t{ 1 } << (index % kBitsPerWord))) != 0;
}

void setBit(std::vector<uint64_t>& bits, std::size_t index)
{
    bits[index / kBitsPerWord] |= uint64_t{ 1 } << (index % kBitsPerWord);
}

void orInto(std::vector<uint64_t>& target, const std::vector<uint64_t>& source)
{
    for (std::size_t i = 0; i < target.size(); ++i)
        target[i] |= source[i];
}

} // namespace

AppSchedules::AppSchedules()
{
    // Simulation phases opt into parallel execution. Extract and the GPU phases
    // stay serial: `render` runs on the render domain, where the RHI contract
    // forbids handing work to Any-affinity workers.
    setExecutionMode(AppSchedule::First, ScheduleExecutionMode::Parallel);
    setExecutionMode(AppSchedule::preUpdate, ScheduleExecutionMode::Parallel);
    setExecutionMode(AppSchedule::update, ScheduleExecutionMode::Parallel);
    setExecutionMode(AppSchedule::PostUpdate, ScheduleExecutionMode::Parallel);
    setExecutionMode(AppSchedule::Last, ScheduleExecutionMode::Parallel);
}

std::size_t AppSchedules::phaseIndex(AppSchedule schedule)
{
    return static_cast<std::size_t>(schedule);
}

AppSchedules& AppSchedules::addSystem(
    AppSchedule schedule,
    SystemLabel label,
    SystemFn system,
    AppSystemOrdering ordering)
{
    return addSystem(
        schedule,
        std::move(label),
        std::move(system),
        ecs::SystemAccess{},
        std::move(ordering));
}

AppSchedules& AppSchedules::addSystem(
    AppSchedule schedule,
    SystemLabel label,
    SystemFn system,
    ecs::SystemAccess access,
    AppSystemOrdering ordering)
{
    PhaseSchedule& phase = m_phases[phaseIndex(schedule)];
    phase.systems.push_back(
        System{ std::move(label), std::move(system), std::move(ordering), std::move(access) });
    phase.planDirty = true;
    return *this;
}

AppSchedules& AppSchedules::configureSets(AppSchedule schedule, SystemLabel earlier, SystemLabel later)
{
    PhaseSchedule& phase = m_phases[phaseIndex(schedule)];
    phase.setOrder.push_back(SetOrderRule{ std::move(earlier), std::move(later), false });
    phase.planDirty = true;
    return *this;
}

AppSchedules& AppSchedules::configureSetAfterOthers(AppSchedule schedule, SystemLabel later)
{
    PhaseSchedule& phase = m_phases[phaseIndex(schedule)];
    phase.setOrder.push_back(SetOrderRule{ {}, std::move(later), true });
    phase.planDirty = true;
    return *this;
}

void AppSchedules::setExecutionMode(AppSchedule schedule, ScheduleExecutionMode mode)
{
    PhaseSchedule& phase = m_phases[phaseIndex(schedule)];
    phase.mode = mode;
    phase.planDirty = true;
}

ScheduleExecutionMode AppSchedules::executionMode(AppSchedule schedule) const
{
    return m_phases[phaseIndex(schedule)].mode;
}

const SchedulePlanInfo& AppSchedules::planInfo(AppSchedule schedule) const
{
    const PhaseSchedule& phase = m_phases[phaseIndex(schedule)];
    ensurePlan(phase);
    return phase.cachedInfo;
}

const std::vector<ecs::SystemNode>& AppSchedules::planNodes(AppSchedule schedule) const
{
    const PhaseSchedule& phase = m_phases[phaseIndex(schedule)];
    ensurePlan(phase);
    return phase.cachedNodes;
}

const std::vector<int>& AppSchedules::planOrder(AppSchedule schedule) const
{
    const PhaseSchedule& phase = m_phases[phaseIndex(schedule)];
    ensurePlan(phase);
    return phase.cachedExecutionOrder;
}

const ecs::SystemExecutorStats& AppSchedules::lastRunStats(AppSchedule schedule) const
{
    return m_phases[phaseIndex(schedule)].lastStats;
}

std::string AppSchedules::describePlan(AppSchedule schedule) const
{
    const SchedulePlanInfo& info = planInfo(schedule);
    char buffer[256];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%s: %u systems (%u exclusive), %u explicit + %u implicit edges, max width %u%s, mode %s",
        toString(schedule),
        info.systemCount,
        info.exclusiveSystemCount,
        info.explicitEdgeCount,
        info.implicitEdgeCount,
        info.maxParallelWidth,
        info.hasCycle ? ", CYCLE" : "",
        executionMode(schedule) == ScheduleExecutionMode::Parallel ? "parallel" : "serial");
    return std::string(buffer);
}

void AddEdge(std::vector<std::vector<int>>& outgoing, std::vector<int>& indegree, int from, int to)
{
    if (from < 0 || to < 0 || from == to)
        return;

    std::vector<int>& edges = outgoing[static_cast<std::size_t>(from)];
    if (std::find(edges.begin(), edges.end(), to) != edges.end())
        return;

    edges.push_back(to);
    ++indegree[static_cast<std::size_t>(to)];
}

namespace
{

// Explicit ordering edges only: runBefore / runAfter plus the set-level rules.
void buildExplicitEdges(
    const std::vector<SystemLabel>& labels,
    const std::vector<AppSystemOrdering>& orderings,
    const std::vector<std::pair<SystemLabel, std::pair<SystemLabel, bool>>>& setRules,
    std::vector<std::vector<int>>& outgoing,
    std::vector<int>& indegree)
{
    const int count = static_cast<int>(labels.size());

    auto findSystemIndex = [&labels](const SystemLabel& label) -> int {
        if (!label.valid())
            return -1;
        const auto it = std::find(labels.begin(), labels.end(), label);
        if (it == labels.end())
            return -1;
        return static_cast<int>(std::distance(labels.begin(), it));
    };

    for (int i = 0; i < count; ++i)
    {
        const AppSystemOrdering& ordering = orderings[static_cast<std::size_t>(i)];
        for (const SystemLabel& before : ordering.before)
            AddEdge(outgoing, indegree, i, findSystemIndex(before));
        for (const SystemLabel& after : ordering.after)
            AddEdge(outgoing, indegree, findSystemIndex(after), i);
    }

    for (const auto& rule : setRules)
    {
        const SystemLabel& earlier = rule.first;
        const SystemLabel& later = rule.second.first;
        const bool earlierIsAnyOutsideLater = rule.second.second;

        std::vector<int> earlierIdx;
        std::vector<int> laterIdx;
        earlierIdx.reserve(static_cast<std::size_t>(count));
        laterIdx.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i)
        {
            const SystemLabel& set = orderings[static_cast<std::size_t>(i)].set;
            if (earlierIsAnyOutsideLater)
            {
                if (set.valid() && set == later)
                    laterIdx.push_back(i);
                else
                    earlierIdx.push_back(i);
            }
            else
            {
                if (set.valid() && set == earlier)
                    earlierIdx.push_back(i);
                if (set.valid() && set == later)
                    laterIdx.push_back(i);
            }
        }
        for (int a : earlierIdx)
            for (int b : laterIdx)
                AddEdge(outgoing, indegree, a, b);
    }
}

// Lowest ready index first, matching the historical serial order exactly.
std::vector<int> topologicalOrder(
    const std::vector<std::vector<int>>& outgoing,
    std::vector<int> indegree,
    bool* hasCycle)
{
    const int count = static_cast<int>(indegree.size());
    std::vector<int> ordered;
    ordered.reserve(indegree.size());
    std::vector<bool> consumed(static_cast<std::size_t>(count), false);

    if (hasCycle)
        *hasCycle = false;

    while (static_cast<int>(ordered.size()) < count)
    {
        int next = -1;
        for (int i = 0; i < count; ++i)
        {
            if (!consumed[static_cast<std::size_t>(i)] && indegree[static_cast<std::size_t>(i)] == 0)
            {
                next = i;
                break;
            }
        }

        if (next < 0)
        {
            if (hasCycle)
                *hasCycle = true;
            for (int i = 0; i < count; ++i)
            {
                if (!consumed[static_cast<std::size_t>(i)])
                    ordered.push_back(i);
            }
            break;
        }

        consumed[static_cast<std::size_t>(next)] = true;
        ordered.push_back(next);
        for (int target : outgoing[static_cast<std::size_t>(next)])
            --indegree[static_cast<std::size_t>(target)];
    }

    return ordered;
}

} // namespace

void AppSchedules::ensurePlan(const PhaseSchedule& phase)
{
    if (!phase.planDirty)
        return;

    buildPlan(phase);
    phase.planDirty = false;
}

void AppSchedules::buildPlan(const PhaseSchedule& phase)
{
    const std::vector<System>& systems = phase.systems;
    const std::size_t count = systems.size();

    phase.cachedNodes.clear();
    phase.cachedExecutionOrder.clear();
    phase.cachedInfo = SchedulePlanInfo{};
    phase.cachedInfo.systemCount = static_cast<uint32_t>(count);
    phase.commandBuffers.clear();
    phase.commandBuffers.resize(count);

    if (count == 0)
        return;

    std::vector<SystemLabel> labels;
    std::vector<AppSystemOrdering> orderings;
    labels.reserve(count);
    orderings.reserve(count);
    uint32_t parallelCount = 0;
    for (const System& system : systems)
    {
        labels.push_back(system.label);
        orderings.push_back(system.ordering);
        if (!system.access.exclusive)
            ++parallelCount;
    }
    phase.cachedInfo.exclusiveSystemCount = static_cast<uint32_t>(count) - parallelCount;

    std::vector<std::pair<SystemLabel, std::pair<SystemLabel, bool>>> setRules;
    setRules.reserve(phase.setOrder.size());
    for (const SetOrderRule& rule : phase.setOrder)
        setRules.emplace_back(rule.earlier, std::make_pair(rule.later, rule.earlierIsAnyOutsideLater));

    std::vector<std::vector<int>> outgoing(count);
    std::vector<int> indegree(count, 0);
    buildExplicitEdges(labels, orderings, setRules, outgoing, indegree);

    for (const std::vector<int>& edges : outgoing)
        phase.cachedInfo.explicitEdgeCount += static_cast<uint32_t>(edges.size());

    bool hasCycle = false;
    std::vector<int> order = topologicalOrder(outgoing, indegree, &hasCycle);
    phase.cachedInfo.hasCycle = hasCycle;

    // Serial phases and phases where every system is exclusive execute in
    // exactly the historical order; there is nothing to overlap, so skip the
    // quadratic conflict pass entirely.
    if (hasCycle || parallelCount == 0 || phase.mode == ScheduleExecutionMode::Serial)
    {
        phase.cachedExecutionOrder = std::move(order);
        phase.cachedInfo.maxParallelWidth = 1;
        return;
    }

    // Transitive closure over the explicit edges, plus its transpose, so an
    // implicit edge can be inserted only between systems that are not already
    // ordered either way.
    const std::size_t words = wordCount(count);
    std::vector<std::vector<uint64_t>> reaches(count, std::vector<uint64_t>(words, 0));
    std::vector<std::vector<uint64_t>> reachedBy(count, std::vector<uint64_t>(words, 0));

    for (auto it = order.rbegin(); it != order.rend(); ++it)
    {
        const auto node = static_cast<std::size_t>(*it);
        for (int successor : outgoing[node])
        {
            setBit(reaches[node], static_cast<std::size_t>(successor));
            orInto(reaches[node], reaches[static_cast<std::size_t>(successor)]);
        }
    }
    for (std::size_t from = 0; from < count; ++from)
    {
        for (std::size_t to = 0; to < count; ++to)
        {
            if (testBit(reaches[from], to))
                setBit(reachedBy[to], from);
        }
    }

    std::vector<uint64_t> forwardSet(words, 0);
    std::vector<uint64_t> backwardSet(words, 0);
    for (std::size_t a = 0; a < count; ++a)
    {
        for (std::size_t b = a + 1; b < count; ++b)
        {
            if (testBit(reaches[a], b) || testBit(reaches[b], a))
                continue;
            if (!systems[a].access.conflictsWith(systems[b].access))
                continue;

            AddEdge(outgoing, indegree, static_cast<int>(a), static_cast<int>(b));
            ++phase.cachedInfo.implicitEdgeCount;

            // Everything that reached a now also reaches b and b's descendants.
            forwardSet = reaches[b];
            setBit(forwardSet, b);
            backwardSet = reachedBy[a];
            setBit(backwardSet, a);

            for (std::size_t u = 0; u < count; ++u)
            {
                if (testBit(backwardSet, u))
                    orInto(reaches[u], forwardSet);
            }
            for (std::size_t v = 0; v < count; ++v)
            {
                if (testBit(forwardSet, v))
                    orInto(reachedBy[v], backwardSet);
            }
        }
    }

    order = topologicalOrder(outgoing, indegree, &hasCycle);
    phase.cachedInfo.hasCycle = hasCycle;
    phase.cachedExecutionOrder = order;

    phase.cachedNodes.resize(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        phase.cachedNodes[i].access = systems[i].access;
        phase.cachedNodes[i].dependents = outgoing[i];
        phase.cachedNodes[i].prerequisiteCount = indegree[i];
    }

    // Longest-path levels give the widest achievable batch.
    std::vector<uint32_t> level(count, 0);
    uint32_t maxLevel = 0;
    for (int index : order)
    {
        const auto node = static_cast<std::size_t>(index);
        for (int successor : outgoing[node])
        {
            const auto target = static_cast<std::size_t>(successor);
            level[target] = std::max(level[target], level[node] + 1);
            maxLevel = std::max(maxLevel, level[target]);
        }
    }
    std::vector<uint32_t> levelWidth(maxLevel + 1, 0);
    for (std::size_t i = 0; i < count; ++i)
        ++levelWidth[level[i]];
    phase.cachedInfo.maxParallelWidth = *std::max_element(levelWidth.begin(), levelWidth.end());
}

void AppSchedules::runSerial(const PhaseSchedule& phase, SystemContext& context) const
{
    for (int index : phase.cachedExecutionOrder)
    {
        const System& system = phase.systems[static_cast<std::size_t>(index)];
        if (system.fn)
            system.fn(context);
    }
}

void AppSchedules::runParallel(const PhaseSchedule& phase, SystemContext& context) const
{
    // Change-tick storages for every written component must exist before workers
    // can mark components changed; creating one lazily would mutate the registry
    // context while other workers read it.
    if (ecs::World* sceneWorld = context.sceneEcs())
    {
        for (const System& system : phase.systems)
            system.access.applyWarmups(*sceneWorld);
    }
    // Same reason, for the deferred queue on the resource world.
    (void)context.world.commands();

    if (phase.commandBuffers.size() != phase.systems.size())
        phase.commandBuffers.resize(phase.systems.size());

    ecs::runSystemsParallel(
        phase.cachedNodes,
        phase.cachedExecutionOrder,
        [&phase, &context](int index) {
            const System& system = phase.systems[static_cast<std::size_t>(index)];
            if (!system.fn)
                return;

            if (system.access.exclusive)
            {
                system.fn(context);
                return;
            }

            SystemContext local = context;
            local.deferredCommands = &phase.commandBuffers[static_cast<std::size_t>(index)];
            system.fn(local);
        },
        &phase.lastStats);

    // Merge in registration order so deferred mutations stay deterministic.
    ecs::CommandQueue& sink = context.world.commands();
    for (ecs::CommandQueue& buffer : phase.commandBuffers)
        sink.append(buffer);
}

void AppSchedules::run(AppSchedule schedule, SystemContext& context) const
{
    const PhaseSchedule& phase = m_phases[phaseIndex(schedule)];
    ensurePlan(phase);

    const bool parallel = m_parallelExecutionEnabled
        && phase.mode == ScheduleExecutionMode::Parallel
        && !phase.cachedInfo.hasCycle
        && !phase.cachedNodes.empty();

    if (parallel)
        runParallel(phase, context);
    else
        runSerial(phase, context);
}

void AppSchedules::clear()
{
    for (PhaseSchedule& phase : m_phases)
    {
        phase.systems.clear();
        phase.setOrder.clear();
        phase.cachedExecutionOrder.clear();
        phase.cachedNodes.clear();
        phase.commandBuffers.clear();
        phase.cachedInfo = SchedulePlanInfo{};
        phase.lastStats = ecs::SystemExecutorStats{};
        phase.planDirty = true;
    }
}

} // namespace caustica

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SceneTransforms.h>
#include <engine/ScenePlugins.h>
#include <engine/SystemLabels.h>
#include <engine/Time.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace
{
bool expect(bool condition, const char* message)
{
    if (condition)
        return true;
    std::fprintf(stderr, "SchedulePlan test failed: %s\n", message);
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

struct SystemA { static constexpr const char* name = "Test.A"; };
struct SystemB { static constexpr const char* name = "Test.B"; };
struct SystemC { static constexpr const char* name = "Test.C"; };
struct SystemD { static constexpr const char* name = "Test.D"; };
struct HostParallelSystem { static constexpr const char* name = "Test.HostParallel"; };

using caustica::AppSchedule;
using caustica::AppSchedules;
using caustica::AppSystemOrdering;
using caustica::ScheduleExecutionMode;
using caustica::SystemContext;
using caustica::ecs::SystemAccess;
using caustica::ecs::SystemNode;

SystemAccess readOnly()
{
    SystemAccess access = SystemAccess::makeParallel();
    access.readComponent<Position>();
    return access;
}

SystemAccess writePosition()
{
    SystemAccess access = SystemAccess::makeParallel();
    access.writeComponent<Position>();
    return access;
}

SystemAccess writeVelocity()
{
    SystemAccess access = SystemAccess::makeParallel();
    access.writeComponent<Velocity>();
    return access;
}

SystemAccess writeHealth()
{
    SystemAccess access = SystemAccess::makeParallel();
    access.writeComponent<Health>();
    return access;
}

// Systems are never invoked here; only the prepared graph is inspected.
caustica::SystemFn noopSystem()
{
    return [](SystemContext&) {};
}

bool dependsOn(const std::vector<SystemNode>& nodes, int from, int to)
{
    const std::vector<int>& dependents = nodes[static_cast<std::size_t>(from)].dependents;
    return std::find(dependents.begin(), dependents.end(), to) != dependents.end();
}

int positionOf(const std::vector<int>& order, int index)
{
    const auto it = std::find(order.begin(), order.end(), index);
    return it == order.end() ? -1 : static_cast<int>(std::distance(order.begin(), it));
}
} // namespace

int main()
{
    bool passed = true;

    // A phase made entirely of exclusive systems keeps the historical serial
    // order and skips graph construction altogether.
    {
        AppSchedules schedules;
        schedules.setExecutionMode(AppSchedule::update, ScheduleExecutionMode::Parallel);
        schedules.addSystem(AppSchedule::update, caustica::systemLabel<SystemA>(), noopSystem());
        schedules.addSystem(
            AppSchedule::update,
            caustica::systemLabel<SystemB>(),
            noopSystem(),
            AppSystemOrdering{}.runBefore<SystemA>());

        const std::vector<int>& order = schedules.planOrder(AppSchedule::update);
        passed &= expect(order.size() == 2, "exclusive phase lost a system");
        passed &= expect(positionOf(order, 1) < positionOf(order, 0),
            "runBefore edge was not honoured");
        passed &= expect(schedules.planNodes(AppSchedule::update).empty(),
            "all-exclusive phase still built a parallel graph");
        passed &= expect(schedules.planInfo(AppSchedule::update).exclusiveSystemCount == 2,
            "exclusive systems were miscounted");
    }

    // Disjoint writers get no edges and can overlap.
    {
        AppSchedules schedules;
        schedules.setExecutionMode(AppSchedule::update, ScheduleExecutionMode::Parallel);
        schedules.addSystem(
            AppSchedule::update, caustica::systemLabel<SystemA>(), noopSystem(), writePosition());
        schedules.addSystem(
            AppSchedule::update, caustica::systemLabel<SystemB>(), noopSystem(), writeVelocity());
        schedules.addSystem(
            AppSchedule::update, caustica::systemLabel<SystemC>(), noopSystem(), writeHealth());

        const auto& info = schedules.planInfo(AppSchedule::update);
        const auto& nodes = schedules.planNodes(AppSchedule::update);
        passed &= expect(info.implicitEdgeCount == 0,
            "disjoint writers were serialized against each other");
        passed &= expect(info.maxParallelWidth == 3, "disjoint writers reported no parallel width");
        passed &= expect(nodes.size() == 3, "parallel phase lost a system");
        for (const SystemNode& node : nodes)
            passed &= expect(node.prerequisiteCount == 0, "an unconstrained system gained a prerequisite");
    }

    // Conflicting systems are ordered by registration order rather than left
    // ambiguous, so the schedule stays reproducible.
    {
        AppSchedules schedules;
        schedules.setExecutionMode(AppSchedule::update, ScheduleExecutionMode::Parallel);
        schedules.addSystem(
            AppSchedule::update, caustica::systemLabel<SystemA>(), noopSystem(), writePosition());
        schedules.addSystem(
            AppSchedule::update, caustica::systemLabel<SystemB>(), noopSystem(), readOnly());
        schedules.addSystem(
            AppSchedule::update, caustica::systemLabel<SystemC>(), noopSystem(), readOnly());

        const auto& info = schedules.planInfo(AppSchedule::update);
        const auto& nodes = schedules.planNodes(AppSchedule::update);
        passed &= expect(info.implicitEdgeCount == 2,
            "writer/reader pairs did not each get an implicit edge");
        passed &= expect(dependsOn(nodes, 0, 1) && dependsOn(nodes, 0, 2),
            "the writer was not ordered before both readers");
        passed &= expect(!dependsOn(nodes, 1, 2) && !dependsOn(nodes, 2, 1),
            "two readers of the same component were serialized");
        passed &= expect(info.maxParallelWidth == 2, "readers were not reported as overlapping");
    }

    // An implicit edge must never close a cycle against an explicit one: here B
    // is declared to run before A even though both write Position.
    {
        AppSchedules schedules;
        schedules.setExecutionMode(AppSchedule::update, ScheduleExecutionMode::Parallel);
        schedules.addSystem(
            AppSchedule::update, caustica::systemLabel<SystemA>(), noopSystem(), writePosition());
        schedules.addSystem(
            AppSchedule::update,
            caustica::systemLabel<SystemB>(),
            noopSystem(),
            writePosition(),
            AppSystemOrdering{}.runBefore<SystemA>());

        const auto& info = schedules.planInfo(AppSchedule::update);
        const auto& order = schedules.planOrder(AppSchedule::update);
        passed &= expect(!info.hasCycle, "the planner introduced a cycle");
        passed &= expect(info.implicitEdgeCount == 0,
            "an implicit edge was added between already-ordered systems");
        passed &= expect(positionOf(order, 1) < positionOf(order, 0),
            "the explicit edge was lost");
    }

    // An exclusive system is ordered against every other system in the phase.
    {
        AppSchedules schedules;
        schedules.setExecutionMode(AppSchedule::update, ScheduleExecutionMode::Parallel);
        schedules.addSystem(
            AppSchedule::update, caustica::systemLabel<SystemA>(), noopSystem(), writePosition());
        schedules.addSystem(AppSchedule::update, caustica::systemLabel<SystemB>(), noopSystem());
        schedules.addSystem(
            AppSchedule::update, caustica::systemLabel<SystemC>(), noopSystem(), writeVelocity());
        schedules.addSystem(
            AppSchedule::update, caustica::systemLabel<SystemD>(), noopSystem(), writeHealth());

        const auto& nodes = schedules.planNodes(AppSchedule::update);
        passed &= expect(nodes.size() == 4, "exclusive-mixed phase lost a system");
        passed &= expect(dependsOn(nodes, 0, 1), "the exclusive system was not ordered after A");
        passed &= expect(dependsOn(nodes, 1, 2) && dependsOn(nodes, 1, 3),
            "the exclusive system was not ordered before the later systems");
        passed &= expect(!dependsOn(nodes, 2, 3) && !dependsOn(nodes, 3, 2),
            "disjoint systems after the exclusive one were serialized");
    }

    // The shapes docs/embedding-cpp.md recommends must actually derive parallel
    // access. These mirror the thin_client sample: a per-frame animation system
    // and a per-frame read-only system.
    {
        using caustica::detail::makeTypedSystemAccess;

        const auto spin = [](caustica::Res<caustica::Time>,
                             caustica::SceneTransforms,
                             caustica::Query<const Position>) {};
        const SystemAccess spinAccess = makeTypedSystemAccess<decltype(spin)>();
        passed &= expect(!spinAccess.exclusive,
            "Res + SceneTransforms + Query was derived as exclusive");

        const auto report = [](caustica::Res<caustica::Time>,
                               caustica::Query<const Velocity>) {};
        const SystemAccess reportAccess = makeTypedSystemAccess<decltype(report)>();
        passed &= expect(!reportAccess.exclusive, "a read-only system was derived as exclusive");
        passed &= expect(!spinAccess.conflictsWith(reportAccess),
            "the sample's per-frame systems cannot overlap");

        // SceneTransforms must still serialize against anything else touching
        // transforms, otherwise its narrow declaration would be a lie.
        SystemAccess transformReader = SystemAccess::makeParallel();
        transformReader.readComponent<caustica::scene::LocalTransformComponent>();
        passed &= expect(spinAccess.conflictsWith(transformReader),
            "SceneTransforms did not declare its LocalTransformComponent write");

        // Taking the whole world stays exclusive.
        const auto setup = [](caustica::EntityWorld) {};
        passed &= expect(makeTypedSystemAccess<decltype(setup)>().exclusive,
            "EntityWorld stopped being exclusive");
        const auto contextual = [](caustica::SystemContext&) {};
        passed &= expect(makeTypedSystemAccess<decltype(contextual)>().exclusive,
            "SystemContext stopped being exclusive");
    }

    // Serial mode never builds a parallel graph even when systems could overlap.
    {
        AppSchedules schedules;
        schedules.setExecutionMode(AppSchedule::update, ScheduleExecutionMode::Serial);
        schedules.addSystem(
            AppSchedule::update, caustica::systemLabel<SystemA>(), noopSystem(), writePosition());
        schedules.addSystem(
            AppSchedule::update, caustica::systemLabel<SystemB>(), noopSystem(), writeVelocity());

        passed &= expect(schedules.planNodes(AppSchedule::update).empty(),
            "serial phase built a parallel graph");
        passed &= expect(schedules.planOrder(AppSchedule::update).size() == 2,
            "serial phase lost a system");
    }

    // Built-in per-frame bookkeeping must not turn the tail of update into an
    // exclusive chain. SceneAnimate remains exclusive, but FPS bookkeeping has
    // precise resource access and can overlap a disjoint host system. The
    // static window title belongs to Startup and adds no update barrier.
    {
        caustica::App app;
        caustica::SceneAnimationPlugin animationPlugin;
        caustica::WindowTitlePlugin windowTitlePlugin;
        animationPlugin.configureSchedules(app);
        windowTitlePlugin.configureSchedules(app);
        app.addSystem<HostParallelSystem>(
            AppSchedule::update,
            [](caustica::Query<Velocity>) {});

        const auto& info = app.schedules().planInfo(AppSchedule::update);
        passed &= expect(info.systemCount == 3,
            "built-in window title still registered in update");
        passed &= expect(info.exclusiveSystemCount == 1,
            "built-in FPS bookkeeping remained exclusive");
        passed &= expect(info.maxParallelWidth >= 2,
            "built-in update tail cannot overlap a disjoint host system");
    }

    return passed ? 0 : 1;
}

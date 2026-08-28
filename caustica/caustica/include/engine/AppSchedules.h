#pragma once

#include <ecs/QueryView.h>
#include <ecs/SystemAccess.h>
#include <ecs/SystemExecutor.h>
#include <ecs/World.h>
#include <engine/SystemLabel.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace caustica
{

class App;
class GpuDevice;

// Convenience alias so systems can take Query<...> without the ecs:: prefix.
template<typename... Components>
using Query = ecs::Query<Components...>;

template<class T>
class Res
{
public:
    explicit Res(const T& resource) : v(&resource) {}

    [[nodiscard]] const T& get() const { return *v; }
    [[nodiscard]] const T* operator->() const { return v; }
    [[nodiscard]] const T& operator*() const { return *v; }

private:
    const T* v;
};

template<class T>
class ResMut
{
public:
    explicit ResMut(T& resource) : v(&resource) {}

    [[nodiscard]] T& get() { return *v; }
    [[nodiscard]] T* operator->() { return v; }
    [[nodiscard]] T& operator*() { return *v; }

private:
    T* v;
};

class Commands
{
public:
    explicit Commands(ecs::CommandQueue& queue) : v(&queue) {}

    [[nodiscard]] ecs::CommandQueue& get() { return *v; }
    [[nodiscard]] ecs::CommandQueue* operator->() { return v; }

private:
    ecs::CommandQueue* v;
};

namespace scene
{
class SceneEntityWorld;
}

enum class AppSchedule
{
    Startup,
    First,
    preUpdate,
    update,
    PostUpdate,
    Extract,
    render,
    postRender,
    Last,
    shutdown,
    Count,
};

[[nodiscard]] const char* toString(AppSchedule schedule);

struct SystemContext
{
    App& app;
    ecs::World& world;
    GpuDevice* gpuDevice = nullptr;
    float deltaTimeSeconds = 0.0f;
    uint32_t frameIndex = 0;
    bool windowFocused = true;
    bool windowVisible = true;

    double elapsedTime = 0.0;
    double currentTime = 0.0;

    bool runUpdate = false;
    bool runRender = false;
    bool abortFrame = false;

    template<typename T>
    [[nodiscard]] T& resMut()
    {
        return world.resource<T>();
    }

    template<typename T>
    [[nodiscard]] const T& res() const
    {
        return world.resource<T>();
    }

    template<typename T>
    [[nodiscard]] T* tryRes()
    {
        return world.getResource<T>();
    }

    template<typename T>
    [[nodiscard]] const T* tryRes() const
    {
        return world.getResource<T>();
    }

    // Private buffer installed by the parallel executor so concurrent systems do
    // not share one queue. Merged back in registration order after the phase.
    ecs::CommandQueue* deferredCommands = nullptr;

    [[nodiscard]] ecs::CommandQueue& commands()
    {
        return deferredCommands ? *deferredCommands : world.commands();
    }

    // Live scene ECS (not App resource world). Prefer over internal GPU digs.
    [[nodiscard]] scene::SceneEntityWorld* entityWorld();
    [[nodiscard]] const scene::SceneEntityWorld* entityWorld() const;
    [[nodiscard]] ecs::World* sceneEcs();
    [[nodiscard]] const ecs::World* sceneEcs() const;
};

using SystemFn = std::function<void(SystemContext&)>;

enum class ScheduleExecutionMode
{
    // One system at a time, in plan order.
    Serial,
    // Systems whose declared access does not overlap run concurrently on
    // TaskRuntime workers (Bevy MultiThreadedExecutor semantics).
    Parallel,
};

// Static description of a prepared phase, for diagnostics and tests.
struct SchedulePlanInfo
{
    uint32_t systemCount = 0;
    uint32_t exclusiveSystemCount = 0;
    // Edges from runBefore / runAfter / set rules.
    uint32_t explicitEdgeCount = 0;
    // Edges the planner inserted between systems with conflicting access, so
    // that mutually exclusive systems keep a stable relative order.
    uint32_t implicitEdgeCount = 0;
    // Widest level of the dependency graph: the best case concurrency.
    uint32_t maxParallelWidth = 1;
    bool hasCycle = false;
};

// Ordered systems for a single AppSchedule phase.
class AppSchedules
{
public:
    AppSchedules();

    // Systems registered through this overload are treated as exclusive: they
    // may reach anything through SystemContext, so they never run beside
    // another system.
    AppSchedules& addSystem(
        AppSchedule schedule,
        SystemLabel label,
        SystemFn system,
        AppSystemOrdering ordering = {});

    AppSchedules& addSystem(
        AppSchedule schedule,
        SystemLabel label,
        SystemFn system,
        ecs::SystemAccess access,
        AppSystemOrdering ordering = {});

    // Set-level edge: every system in `earlier` runs before every system in `later`.
    AppSchedules& configureSets(AppSchedule schedule, SystemLabel earlier, SystemLabel later);

    // Every system not in `later` runs before every system in `later`.
    AppSchedules& configureSetAfterOthers(AppSchedule schedule, SystemLabel later);

    void setExecutionMode(AppSchedule schedule, ScheduleExecutionMode mode);
    [[nodiscard]] ScheduleExecutionMode executionMode(AppSchedule schedule) const;

    // Global kill switch; forces every phase back to Serial when disabled.
    void setParallelExecutionEnabled(bool enabled) { m_parallelExecutionEnabled = enabled; }
    [[nodiscard]] bool parallelExecutionEnabled() const { return m_parallelExecutionEnabled; }

    [[nodiscard]] const SchedulePlanInfo& planInfo(AppSchedule schedule) const;
    // Prepared graph and its dispatch order. Empty node list means the phase
    // falls back to serial execution.
    [[nodiscard]] const std::vector<ecs::SystemNode>& planNodes(AppSchedule schedule) const;
    [[nodiscard]] const std::vector<int>& planOrder(AppSchedule schedule) const;
    [[nodiscard]] const ecs::SystemExecutorStats& lastRunStats(AppSchedule schedule) const;
    [[nodiscard]] std::string describePlan(AppSchedule schedule) const;

    void run(AppSchedule schedule, SystemContext& context) const;
    void clear();

private:
    struct System
    {
        SystemLabel label;
        SystemFn fn;
        AppSystemOrdering ordering;
        ecs::SystemAccess access;
    };

    struct SetOrderRule
    {
        SystemLabel earlier{};
        SystemLabel later{};
        bool earlierIsAnyOutsideLater = false;
    };

    struct PhaseSchedule
    {
        std::vector<System> systems;
        std::vector<SetOrderRule> setOrder;
        ScheduleExecutionMode mode = ScheduleExecutionMode::Serial;
        mutable std::vector<int> cachedExecutionOrder;
        mutable std::vector<ecs::SystemNode> cachedNodes;
        mutable std::vector<ecs::CommandQueue> commandBuffers;
        mutable SchedulePlanInfo cachedInfo;
        mutable ecs::SystemExecutorStats lastStats;
        mutable bool planDirty = true;
    };

    [[nodiscard]] static std::size_t phaseIndex(AppSchedule schedule);
    static void buildPlan(const PhaseSchedule& phase);
    static void ensurePlan(const PhaseSchedule& phase);

    void runSerial(const PhaseSchedule& phase, SystemContext& context) const;
    void runParallel(const PhaseSchedule& phase, SystemContext& context) const;

    PhaseSchedule m_phases[static_cast<std::size_t>(AppSchedule::Count)];
    bool m_parallelExecutionEnabled = true;
};

} // namespace caustica

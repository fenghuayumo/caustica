#pragma once

#include <ecs/World.h>
#include <engine/SystemLabel.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace caustica
{

class App;
class GpuDevice;

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

    [[nodiscard]] ecs::CommandQueue& commands()
    {
        return world.commands();
    }

    // Live scene ECS (not App resource world). Prefer over GpuRenderSubsystem digs.
    [[nodiscard]] scene::SceneEntityWorld* entityWorld();
    [[nodiscard]] const scene::SceneEntityWorld* entityWorld() const;
    [[nodiscard]] ecs::World* sceneEcs();
    [[nodiscard]] const ecs::World* sceneEcs() const;
};

using SystemFn = std::function<void(SystemContext&)>;

// Ordered systems for a single AppSchedule phase.
class AppSchedules
{
public:
    AppSchedules& addSystem(
        AppSchedule schedule,
        SystemLabel label,
        SystemFn system,
        AppSystemOrdering ordering = {});

    // Set-level edge: every system in `earlier` runs before every system in `later`.
    AppSchedules& configureSets(AppSchedule schedule, SystemLabel earlier, SystemLabel later);

    // Every system not in `later` runs before every system in `later`.
    AppSchedules& configureSetAfterOthers(AppSchedule schedule, SystemLabel later);

    void run(AppSchedule schedule, SystemContext& context) const;
    void clear();

private:
    struct System
    {
        SystemLabel label;
        SystemFn fn;
        AppSystemOrdering ordering;
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
    };

    [[nodiscard]] static std::size_t phaseIndex(AppSchedule schedule);
    [[nodiscard]] static std::vector<int> buildExecutionOrder(const PhaseSchedule& phase);

    PhaseSchedule m_phases[static_cast<std::size_t>(AppSchedule::Count)];
};

} // namespace caustica

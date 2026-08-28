#pragma once

#include <ecs/QueryFilters.h>
#include <ecs/TypeId.h>
#include <ecs/World.h>

#include <algorithm>
#include <type_traits>
#include <vector>

namespace caustica::ecs
{

// What one system touches, in the Bevy sense: component and resource reads and
// writes. Two systems may run at the same time only when neither writes
// something the other reads or writes.
//
// `exclusive` is the safe default. A system that takes the raw SystemContext,
// the World, or the SceneEntityWorld can reach anything, so the scheduler runs
// it alone on the scheduling thread.
struct SystemAccess
{
    using WarmupFn = void (*)(World&);

    AccessMask componentReads;
    AccessMask componentWrites;
    AccessMask resourceReads;
    AccessMask resourceWrites;

    bool exclusive = true;
    // Mutates the world only through a deferred CommandQueue.
    bool deferred = false;

    // Change-tick storages that must be created before workers can mark
    // components changed concurrently. Creating one lazily would mutate the
    // registry context under other readers.
    std::vector<WarmupFn> componentWarmups;

    template<typename T>
    void readComponent()
    {
        componentReads.set(typeId<T>());
    }

    template<typename T>
    void writeComponent()
    {
        using Raw = std::remove_cv_t<T>;
        componentWrites.set(typeId<Raw>());
        addWarmup(+[](World& world) { world.ensureChangeTicks<Raw>(); });
    }

    template<typename T>
    void readResource()
    {
        resourceReads.set(typeId<T>());
    }

    template<typename T>
    void writeResource()
    {
        resourceWrites.set(typeId<T>());
    }

    [[nodiscard]] bool conflictsWith(const SystemAccess& other) const
    {
        if (exclusive || other.exclusive)
            return true;

        if (componentWrites.intersects(other.componentWrites)
            || componentWrites.intersects(other.componentReads)
            || componentReads.intersects(other.componentWrites))
            return true;

        return resourceWrites.intersects(other.resourceWrites)
            || resourceWrites.intersects(other.resourceReads)
            || resourceReads.intersects(other.resourceWrites);
    }

    [[nodiscard]] bool touchesAnything() const
    {
        return exclusive
            || !componentReads.empty()
            || !componentWrites.empty()
            || !resourceReads.empty()
            || !resourceWrites.empty();
    }

    void merge(const SystemAccess& other)
    {
        componentReads.merge(other.componentReads);
        componentWrites.merge(other.componentWrites);
        resourceReads.merge(other.resourceReads);
        resourceWrites.merge(other.resourceWrites);
        exclusive = exclusive || other.exclusive;
        deferred = deferred || other.deferred;
        for (WarmupFn warmup : other.componentWarmups)
            addWarmup(warmup);
    }

    void applyWarmups(World& world) const
    {
        for (WarmupFn warmup : componentWarmups)
            warmup(world);
    }

    // A default-constructed SystemAccess is already exclusive; this is the only
    // way to opt into overlapping, and callers must then declare what they touch.
    static SystemAccess makeParallel()
    {
        SystemAccess access;
        access.exclusive = false;
        return access;
    }

private:
    void addWarmup(WarmupFn warmup)
    {
        if (std::find(componentWarmups.begin(), componentWarmups.end(), warmup)
            == componentWarmups.end())
            componentWarmups.push_back(warmup);
    }
};

namespace detail
{

// Maps one Query<...> term onto an access declaration: filters and const fetches
// are reads, a plain (mutable) fetch is a write.
template<typename Term>
void applyQueryTermAccess(SystemAccess& access)
{
    if constexpr (is_query_filter_v<Term>)
        access.template readComponent<typename Term::Type>();
    else if constexpr (std::is_const_v<Term>)
        access.template readComponent<std::remove_const_t<Term>>();
    else
        access.template writeComponent<Term>();
}

} // namespace detail

} // namespace caustica::ecs

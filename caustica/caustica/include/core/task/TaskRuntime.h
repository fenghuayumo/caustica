#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

// =============================================================================
// caustica::task — product-facing TaskRuntime (ADR 0001)
//
// Sole scheduler hub for background / parallel work.
//   Affinity::Any    — worker pool
//   Affinity::Logic  — pumpLogic() on the Logic thread (App update)
//   Affinity::Render — pumpRender() on the render domain
//   Affinity::IO     — dedicated IO worker(s)
// Well-known pipes (created at initialize): "LoadSession", "Logic", "RHI.Submit".
// =============================================================================

namespace caustica::task
{

enum class Priority : uint8_t
{
    Critical = 0,
    High = 1,
    Normal = 2,
    Low = 3,
    Background = 4,
    Count
};

enum class Affinity : uint8_t
{
    Any = 0,
    Logic,
    Render,
    IO,
};

class Pipe;

struct TaskHandle
{
    explicit operator bool() const noexcept { return static_cast<bool>(m_state); }

    friend bool operator==(const TaskHandle& a, const TaskHandle& b) noexcept
    {
        return a.m_state == b.m_state;
    }
    friend bool operator!=(const TaskHandle& a, const TaskHandle& b) noexcept
    {
        return !(a == b);
    }

private:
    friend TaskHandle create(struct TaskDesc);
    friend void submit(TaskHandle);
    friend void then(TaskHandle, TaskHandle);
    friend void wait(TaskHandle);
    friend bool poll(TaskHandle);

    std::shared_ptr<void> m_state;
};

// P1.1 fixed job — preferred over std::function when the callee is a plain function.
using TaskFn = void (*)(void* user);

struct TaskDesc
{
    const char* name = nullptr;
    Priority priority = Priority::Normal;
    Affinity affinity = Affinity::Any;
    Pipe* pipe = nullptr;
    // 0 = immortal. Non-zero: skipped (but still completed for deps) when the
    // matching generation counter has advanced past this value.
    uint64_t generation = 0;
    enum class GenerationDomain : uint8_t { Frame, Load } generationDomain =
        GenerationDomain::Frame;

    // Prefer fn when set; otherwise body (legacy / capture-heavy work).
    TaskFn fn = nullptr;
    void* user = nullptr;
    std::function<void()> body;
};

struct RuntimeStats
{
    uint32_t anyQueued = 0;
    uint32_t logicQueued = 0;
    uint32_t renderQueued = 0;
    uint32_t ioQueued = 0;
    uint32_t workerCount = 0;
    uint32_t ioWorkerCount = 0;
    uint64_t frameGeneration = 0;
    uint64_t loadGeneration = 0;
};

void initialize(uint32_t numWorkers = 0, uint32_t reservedThreads = 1);
void shutdown();
[[nodiscard]] bool isInitialized();
[[nodiscard]] uint32_t workerCount();

[[nodiscard]] Pipe* getPipe(const char* name);
// Well-known pipes (non-null after initialize).
[[nodiscard]] Pipe* loadSessionPipe();
[[nodiscard]] Pipe* logicPipe();
[[nodiscard]] Pipe* rhiSubmitPipe();

[[nodiscard]] TaskHandle create(TaskDesc desc);
void submit(TaskHandle handle);
[[nodiscard]] TaskHandle launch(TaskDesc desc);

// Convenience: fixed-job launch without filling TaskDesc by hand.
[[nodiscard]] TaskHandle launch(
    const char* name,
    Priority priority,
    Affinity affinity,
    TaskFn fn,
    void* user,
    Pipe* pipe = nullptr,
    uint64_t generation = 0,
    TaskDesc::GenerationDomain generationDomain = TaskDesc::GenerationDomain::Frame);

void then(TaskHandle prerequisite, TaskHandle subsequent);

void wait(TaskHandle);
[[nodiscard]] bool poll(TaskHandle);

void helpOnce();

// --- Domain pumps ----------------------------------------------------------
void setRenderWake(std::function<void()> wake);
void pumpRender();
[[nodiscard]] bool isRenderDomainBusy();

// Call from the Logic thread once per App update tick.
void pumpLogic();
[[nodiscard]] bool isLogicDomainBusy();

[[nodiscard]] bool isIoDomainBusy();

[[nodiscard]] RuntimeStats snapshotStats();

[[nodiscard]] uint64_t frameGeneration();
void bumpFrameGeneration();
[[nodiscard]] uint64_t loadGeneration();
void bumpLoadGeneration();

// Stamp current Load generation onto a desc (scene-switch cancellation).
inline void stampLoadGeneration(TaskDesc& desc)
{
    desc.generation = loadGeneration();
    desc.generationDomain = TaskDesc::GenerationDomain::Load;
}

struct Group
{
    std::atomic<uint32_t> pending{0};
};

void launch(Group& group, TaskDesc desc);
void wait(Group& group);
[[nodiscard]] bool isBusy(const Group& group);

void parallelFor(Group& group,
                 uint32_t count,
                 Priority priority,
                 Affinity affinity,
                 std::function<void(uint32_t index)> fn,
                 uint32_t groupSize = 1);

inline void parallelFor(uint32_t count,
                        Priority priority,
                        Affinity affinity,
                        std::function<void(uint32_t index)> fn,
                        uint32_t groupSize = 1)
{
    Group group;
    parallelFor(group, count, priority, affinity, std::move(fn), groupSize);
    wait(group);
}

} // namespace caustica::task

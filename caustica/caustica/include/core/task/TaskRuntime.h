#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

// =============================================================================
// caustica::task — product-facing TaskRuntime (ADR 0001)
//
// Sole scheduler hub for background / parallel work.
// Affinity::Any (+ optional Pipe) runs on worker threads.
// Affinity::Render is pumped on the render domain via pumpRender() (ADR 0001 P2)
// — call from RenderThread::threadMain and sync executeRenderPhase.
// Affinity::Logic / IO currently share Any workers (domain pumps TBD).
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
    std::function<void()> body;
};

void initialize(uint32_t numWorkers = 0, uint32_t reservedThreads = 1);
void shutdown();
[[nodiscard]] bool isInitialized();
[[nodiscard]] uint32_t workerCount();

[[nodiscard]] Pipe* getPipe(const char* name);

[[nodiscard]] TaskHandle create(TaskDesc desc);
void submit(TaskHandle handle);
[[nodiscard]] TaskHandle launch(TaskDesc desc);

void then(TaskHandle prerequisite, TaskHandle subsequent);

void wait(TaskHandle handle);
[[nodiscard]] bool poll(TaskHandle handle);

void helpOnce();

// --- Render domain queue (Affinity::Render) --------------------------------
// Optional wake when a Render task is queued (e.g. RenderThread::wake).
void setRenderWake(std::function<void()> wake);
// Run pending Affinity::Render tasks on the calling thread (must be render domain).
void pumpRender();
[[nodiscard]] bool isRenderDomainBusy();

[[nodiscard]] uint64_t frameGeneration();
void bumpFrameGeneration();
[[nodiscard]] uint64_t loadGeneration();
void bumpLoadGeneration();

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

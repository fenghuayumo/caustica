#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

// =============================================================================
// JobSystem — work-stealing thread pool with parallel dispatch.
//
// Replaces the older ThreadPool with:
//   - Named Contexts for grouping and waiting on related work
//   - Dispatch() for data-parallel workloads (like HLSL Dispatch)
//   - Execute() for async single jobs
//   - Lightweight: jobs are lock-free where possible
//
// Usage:
//   JobSystem::Initialize();            // once at app startup
//   JobSystem::Context ctx;
//   JobSystem::Execute(ctx, [] { ... });
//   JobSystem::Dispatch(ctx, 100, 10, [](JobDispatchArgs args) { ... });
//   JobSystem::Wait(ctx);
//   JobSystem::Shutdown();             // once at app exit
// =============================================================================

struct JobDispatchArgs
{
    uint32_t jobIndex;      // global job index within the dispatch
    uint32_t groupID;       // which group this job belongs to (like SV_GroupID)
    uint32_t groupIndex;    // index within the group (like SV_GroupIndex)
    bool     isFirstJobInGroup;
    bool     isLastJobInGroup;
};

namespace caustica
{
namespace JobSystem
{

// One-time initialization. Creates worker threads.
//   numThreads: 0 = use hardware concurrency - reservedThreads
void Initialize(uint32_t numThreads = 0, uint32_t reservedThreads = 1);

// Shuts down all threads. Waits for outstanding work.
void Shutdown();

// Returns the number of worker threads.
uint32_t GetThreadCount();

// ==========================================================================
// Context — tracks a group of submitted jobs for waiting.
// ==========================================================================
struct Context
{
    std::atomic<uint32_t> counter{0};
};

// Submit a single job for asynchronous execution.
void Execute(Context& ctx, std::function<void()> task);

// Dispatch a parallel-for workload across worker threads.
//   jobCount: total number of work items
//   groupSize: items per group (groups execute serially within a thread)
//   task: receives JobDispatchArgs describing which item to process
void Dispatch(Context& ctx, uint32_t jobCount, uint32_t groupSize,
              std::function<void(JobDispatchArgs)> task);

// Returns the number of groups for a given jobCount and groupSize.
uint32_t GetGroupCount(uint32_t jobCount, uint32_t groupSize);

// Returns true if any jobs in this context are still running.
bool IsBusy(const Context& ctx);

// Block until all jobs submitted to this context have completed.
void Wait(const Context& ctx);

// ==========================================================================
// Convenience: parallel-for over a range.
// ==========================================================================
template <typename F>
void ParallelFor(Context& ctx, uint32_t count, F&& func)
{
    if (count == 0) return;
    constexpr uint32_t kGroupSize = 64;
    Dispatch(ctx, count, kGroupSize,
             [func = std::forward<F>(func)](JobDispatchArgs args) {
                 func(args.jobIndex);
             });
}

} // namespace JobSystem
} // namespace caustica

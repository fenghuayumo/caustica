/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace caustica {
namespace core {

// Lightweight job system for dispatching work to thread pool.
// Wraps the existing caustica::ThreadPool in a simpler API.
class JobSystem
{
public:
    // Initialize the job system.
    // `reservedThreads` = number of threads to reserve (e.g. 2 for main + render).
    // Actual worker count = max(1, hardware_concurrency - reservedThreads).
    static void init(uint32_t reservedThreads = 2);
    static void shutdown();

    // Execute a batch of jobs and wait for completion.
    // Each job function receives a JobArgs with its index and group index.
    struct JobArgs
    {
        uint32_t jobIndex;
        uint32_t groupIndex;
    };

    using JobFunc = std::function<void(JobArgs)>;

    // Dispatch `count` jobs; each calls `func` with its JobArgs.
    static void dispatch(uint32_t count, JobFunc func);

    // Convenience: dispatch a single job
    static void dispatchOne(JobFunc func);

    // Wait for all dispatched jobs to complete
    static void wait();

    // Number of worker threads
    static uint32_t numWorkers();

private:
    struct JobItem
    {
        JobFunc func;
        JobArgs args;
    };

    static void workerLoop();

    static std::vector<std::thread> s_Threads;
    static std::queue<JobItem>      s_JobQueue;
    static std::mutex               s_Mutex;
    static std::condition_variable  s_Condition;
    static std::atomic<bool>        s_Running;
    static std::atomic<uint32_t>    s_PendingJobs;
};

// RAII helper: dispatch jobs, auto-wait on destruction
struct JobContext
{
    ~JobContext() { JobSystem::wait(); }
};

} // namespace core
} // namespace caustica

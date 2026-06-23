/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "core/job_system.h"

#include <algorithm>

namespace caustica {
namespace core {

std::vector<std::thread>     JobSystem::s_Threads;
std::queue<JobSystem::JobItem> JobSystem::s_JobQueue;
std::mutex                   JobSystem::s_Mutex;
std::condition_variable      JobSystem::s_Condition;
std::atomic<bool>            JobSystem::s_Running{false};
std::atomic<uint32_t>        JobSystem::s_PendingJobs{0};

void JobSystem::init(uint32_t reservedThreads)
{
    if (s_Running.load())
        return;

    uint32_t hwThreads = std::thread::hardware_concurrency();
    uint32_t numThreads = (hwThreads > reservedThreads) ? (hwThreads - reservedThreads) : 1;

    s_Running.store(true);

    s_Threads.resize(numThreads);
    for (uint32_t i = 0; i < numThreads; ++i)
    {
        s_Threads[i] = std::thread(workerLoop);
    }
}

void JobSystem::shutdown()
{
    s_Running.store(false);
    s_Condition.notify_all();

    for (auto& t : s_Threads)
    {
        if (t.joinable())
            t.join();
    }
    s_Threads.clear();
}

void JobSystem::dispatch(uint32_t count, JobFunc func)
{
    if (count == 0)
        return;

    s_PendingJobs.fetch_add(count);

    {
        std::lock_guard<std::mutex> lock(s_Mutex);
        for (uint32_t i = 0; i < count; ++i)
        {
            s_JobQueue.push({func, {i, 0}});
        }
    }

    s_Condition.notify_all();
}

void JobSystem::dispatchOne(JobFunc func)
{
    dispatch(1, std::move(func));
}

void JobSystem::wait()
{
    while (s_PendingJobs.load() != 0)
    {
        // Help out: process jobs on the calling thread
        JobItem item;
        {
            std::lock_guard<std::mutex> lock(s_Mutex);
            if (!s_JobQueue.empty())
            {
                item = std::move(s_JobQueue.front());
                s_JobQueue.pop();
            }
        }

        if (item.func)
        {
            item.func(item.args);
            s_PendingJobs.fetch_sub(1);
        }
        else
        {
            std::this_thread::yield();
        }
    }
}

uint32_t JobSystem::numWorkers()
{
    return static_cast<uint32_t>(s_Threads.size());
}

void JobSystem::workerLoop()
{
    while (s_Running.load())
    {
        JobItem item;
        {
            std::unique_lock<std::mutex> lock(s_Mutex);
            s_Condition.wait(lock, []{
                return !s_JobQueue.empty() || !s_Running.load();
            });

            if (!s_JobQueue.empty())
            {
                item = std::move(s_JobQueue.front());
                s_JobQueue.pop();
            }
        }

        if (item.func)
        {
            item.func(item.args);
            s_PendingJobs.fetch_sub(1);
        }
    }
}

} // namespace core
} // namespace caustica

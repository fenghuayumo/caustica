#pragma once

#include <core/JobSystem.h>
#include <functional>
#include <memory>

namespace caustica
{

// ==========================================================================
// ThreadPoolTask — legacy interface (kept for backward compatibility)
// ==========================================================================
class ThreadPoolTask
{
public:
    virtual void Run() = 0;
};

// ==========================================================================
// ThreadPool — now a thin wrapper around JobSystem.
//
// Existing code continues to work unchanged.  New code should use
// JobSystem::execute / JobSystem::dispatch directly.
// ==========================================================================
class ThreadPool
{
public:
    ThreadPool(uint32_t numThreads = 0);
    ~ThreadPool();

    void AddTask(std::shared_ptr<ThreadPoolTask> const& task);
    void AddTask(std::function<void()> func);
    void WaitForTasks();

private:
    JobSystem::Context m_Context;
};

}
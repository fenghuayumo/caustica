#include <core/ThreadPool.h>

namespace caustica
{

// ==========================================================================
// ThreadPool is now a thin wrapper around JobSystem.
// The global JobSystem must be initialized before creating ThreadPool instances.
// ==========================================================================

ThreadPool::ThreadPool(uint32_t /*numThreads*/)
{
    // numThreads is ignored — the global JobSystem manages thread count.
    // Call JobSystem::Initialize(numThreads) at app startup to configure.
}

ThreadPool::~ThreadPool()
{
    WaitForTasks();
}

void ThreadPool::AddTask(std::shared_ptr<ThreadPoolTask> const& task)
{
    // Capture the shared_ptr to keep the task alive
    JobSystem::Execute(m_Context, [task]() {
        task->Run();
    });
}

void ThreadPool::AddTask(std::function<void()> func)
{
    JobSystem::Execute(m_Context, std::move(func));
}

void ThreadPool::WaitForTasks()
{
    JobSystem::Wait(m_Context);
}

}
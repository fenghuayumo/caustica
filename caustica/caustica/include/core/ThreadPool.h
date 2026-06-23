#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

using namespace std::chrono;

namespace caustica
{

class ThreadPoolTask
{
public:
    // Execute the task.
    virtual void Run() = 0;
};

class ThreadPool
{
public:
    ThreadPool(uint32_t numThreads = 0);
    ~ThreadPool();

    // Enqueues a task for execution in the thread pool.
    // If any thread is available, the task immediately starts executing.
    void AddTask(std::shared_ptr<ThreadPoolTask> const& task);

    // Enqueues a function for execution in the thread pool.
    // If any thread is available, the function immediately starts executing.
    void AddTask(std::function<void()> func);

    // Waits for all previously added tasks to complete or fail.
    void WaitForTasks();

private:
    static void StaticThreadProc(ThreadPool* self);
    void ThreadProc();

    std::vector<std::thread> m_threads;
    std::queue<std::shared_ptr<ThreadPoolTask>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_forward;
    std::atomic<bool> m_terminate = false;
    std::atomic<int> m_pendingTasks = 0;
};

}
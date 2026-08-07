#include "scene/SceneLoader.h"

#include <memory>
#include <utility>

namespace caustica
{

struct ImportJob
{
    SceneLoader* loader = nullptr;
    std::shared_ptr<IFileSystem> fs;
    std::filesystem::path path;
    uint64_t generation = 0;

    static void run(void* user)
    {
        std::unique_ptr<ImportJob> job(static_cast<ImportJob*>(user));
        if (!job->loader)
            return;

        if (job->generation != task::loadGeneration())
        {
            job->loader->notifyLoadFinished(false);
            return;
        }

        const bool ok = job->loader->invokeLoadFunc(std::move(job->fs), job->path);
        job->loader->notifyLoadFinished(ok);
    }
};

SceneLoader::~SceneLoader()
{
    if (m_task)
        task::wait(m_task);
}

void SceneLoader::beginLoading(std::shared_ptr<IFileSystem> fs,
    const std::filesystem::path& path)
{
    if (m_loaded.load(std::memory_order_acquire) && onUnloading)
        onUnloading();

    reset();

    if (!m_loadFunc)
        return;

    if (m_asyncLoad)
    {
        auto job = std::make_unique<ImportJob>();
        job->loader = this;
        job->fs = std::move(fs);
        job->path = path;
        job->generation = task::loadGeneration();
        m_task = task::launch(
            "LoadSession.Import",
            task::Priority::Background,
            task::Affinity::IO,
            &ImportJob::run,
            job.release(),
            task::loadSessionPipe());
    }
    else
    {
        const bool ok = m_loadFunc(std::move(fs), path);
        notifyLoadFinished(ok);
    }
}

void SceneLoader::update()
{
    if (!m_task)
        return;
    if (!task::poll(m_task))
        return;

    m_task = {};

    if (m_loaded.load(std::memory_order_acquire) && onLoaded)
        onLoaded();
}

void SceneLoader::reset()
{
    if (m_task)
    {
        task::wait(m_task);
        m_task = {};
    }
    m_loaded.store(false, std::memory_order_release);
    m_loadFinished.store(false, std::memory_order_release);
}

bool SceneLoader::invokeLoadFunc(
    std::shared_ptr<IFileSystem> fs,
    const std::filesystem::path& path)
{
    return m_loadFunc ? m_loadFunc(std::move(fs), path) : false;
}

void SceneLoader::notifyLoadFinished(bool ok)
{
    m_loaded.store(ok, std::memory_order_release);
    m_loadFinished.store(true, std::memory_order_release);
}

} // namespace caustica

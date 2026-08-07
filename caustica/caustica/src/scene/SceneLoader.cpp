#include "scene/SceneLoader.h"

#include <atomic>

namespace caustica
{

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
        task::TaskDesc desc;
        desc.name = "LoadSession.Import";
        desc.priority = task::Priority::Background;
        desc.affinity = task::Affinity::IO;
        desc.pipe = task::loadSessionPipe();
        task::stampLoadGeneration(desc);
        desc.body = [this, fs = std::move(fs), path]() mutable {
            const bool ok = m_loadFunc(std::move(fs), path);
            m_loaded.store(ok, std::memory_order_release);
            // Always signal completion so update() can clear failed loads too.
            m_loadFinished.store(true, std::memory_order_release);
        };
        m_task = task::launch(std::move(desc));
    }
    else
    {
        const bool ok = m_loadFunc(std::move(fs), path);
        m_loaded.store(ok, std::memory_order_release);
        m_loadFinished.store(true, std::memory_order_release);
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

} // namespace caustica

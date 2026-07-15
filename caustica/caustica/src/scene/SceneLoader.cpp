#include "scene/SceneLoader.h"

#include <atomic>

namespace caustica
{

SceneLoader::~SceneLoader()
{
    if (m_thread && m_thread->joinable())
        m_thread->join();
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
        m_thread = std::make_unique<std::thread>(
            [this, fs = std::move(fs), path]() mutable
            {
                const bool ok = m_loadFunc(std::move(fs), path);
                m_loaded.store(ok, std::memory_order_release);
                // Always signal completion so update() can join failed loads too.
                m_loadFinished.store(true, std::memory_order_release);
            });
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
    if (m_loadFinished.load(std::memory_order_acquire) && m_thread && m_thread->joinable())
    {
        m_thread->join();
        m_thread = nullptr;

        if (m_loaded.load(std::memory_order_acquire) && onLoaded)
            onLoaded();
    }
}

void SceneLoader::reset()
{
    if (m_thread && m_thread->joinable())
        m_thread->join();
    m_thread = nullptr;
    m_loaded.store(false, std::memory_order_release);
    m_loadFinished.store(false, std::memory_order_release);
}

} // namespace caustica

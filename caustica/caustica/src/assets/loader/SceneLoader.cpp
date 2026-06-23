#include "assets/loader/SceneLoader.h"

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
    if (m_loaded && onUnloading)
        onUnloading();

    reset();

    if (!m_loadFunc)
        return;

    if (m_asyncLoad)
    {
        m_thread = std::make_unique<std::thread>(
            [this, fs = std::move(fs), path]() mutable
            {
                m_loaded = m_loadFunc(std::move(fs), path);
            });
    }
    else
    {
        m_loaded = m_loadFunc(std::move(fs), path);
    }
}

void SceneLoader::update()
{
    if (m_loaded && m_thread && m_thread->joinable())
    {
        m_thread->join();
        m_thread = nullptr;

        if (onLoaded)
            onLoaded();
    }
}

void SceneLoader::reset()
{
    if (m_thread && m_thread->joinable())
        m_thread->join();
    m_thread = nullptr;
    m_loaded = false;
}

} // namespace caustica

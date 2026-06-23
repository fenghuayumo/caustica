#include "engine/scene_manager.h"

// Include the complete existing Scene/SceneGraph types only here
#include <engine/Scene.h>
#include <engine/SceneGraph.h>

#include <algorithm>
#include <cstdio>

namespace caustica
{

struct SceneManager::Impl
{
    std::vector<std::shared_ptr<Scene>> scenes;
    Scene*  currentScene   = nullptr;
    uint32_t currentIndex  = 0;
    int     queuedIndex    = -1;
    bool    switching      = false;

    std::vector<std::string> sceneFilePaths;
    std::vector<std::string> filesToLoad;
};

SceneManager::SceneManager()
    : m_Pimpl(std::make_unique<Impl>())
{}

SceneManager::~SceneManager() = default;

bool SceneManager::isSwitching() const { return m_Pimpl->switching; }

Scene* SceneManager::getCurrentScene() const { return m_Pimpl->currentScene; }

uint32_t SceneManager::getCurrentSceneIndex() const { return m_Pimpl->currentIndex; }

uint32_t SceneManager::getSceneCount() const
{
    return static_cast<uint32_t>(m_Pimpl->scenes.size());
}

void SceneManager::switchScene(int index)
{
    if (index >= 0 && static_cast<size_t>(index) < m_Pimpl->scenes.size())
    {
        m_Pimpl->queuedIndex = index;
        m_Pimpl->switching   = true;
    }
}

void SceneManager::switchScene(const std::string& name)
{
    for (size_t i = 0; i < m_Pimpl->scenes.size(); ++i)
    {
        if (m_Pimpl->scenes[i])
        {
            switchScene(static_cast<int>(i));
            return;
        }
    }
}

void SceneManager::applySceneSwitch()
{
    auto& p = *m_Pimpl;
    if (!p.switching || p.queuedIndex < 0) return;

    if (static_cast<size_t>(p.queuedIndex) < p.scenes.size())
    {
        p.currentScene = p.scenes[p.queuedIndex].get();
        p.currentIndex = static_cast<uint32_t>(p.queuedIndex);
    }
    p.queuedIndex = -1;
    p.switching   = false;
}

void SceneManager::enqueueScene(Scene* scene)
{
    if (scene)
    {
        m_Pimpl->scenes.emplace_back(std::shared_ptr<Scene>(scene));
        if (!m_Pimpl->currentScene)
        {
            m_Pimpl->currentScene = scene;
            m_Pimpl->currentIndex = static_cast<uint32_t>(m_Pimpl->scenes.size() - 1);
        }
    }
}

void SceneManager::removeScene(int index)
{
    auto& p = *m_Pimpl;
    if (index < 0 || static_cast<size_t>(index) >= p.scenes.size()) return;

    if (p.currentScene == p.scenes[index].get())
        p.currentScene = nullptr;

    p.scenes.erase(p.scenes.begin() + index);

    if (!p.currentScene && !p.scenes.empty())
    {
        p.currentScene = p.scenes[0].get();
        p.currentIndex = 0;
    }
}

std::vector<std::string> SceneManager::getSceneNames() const
{
    std::vector<std::string> names;
    names.reserve(m_Pimpl->scenes.size());
    for (auto& s : m_Pimpl->scenes)
    {
        if (s)
            names.push_back("Scene");  // Will use Scene::GetName() once wired
    }
    return names;
}

void SceneManager::enqueueSceneFromFile(const std::string& filePath)
{
    m_Pimpl->filesToLoad.push_back(filePath);
    m_Pimpl->sceneFilePaths.push_back(filePath);
}

void SceneManager::addFileToLoadList(const std::string& filePath)
{
    m_Pimpl->filesToLoad.push_back(filePath);
}

void SceneManager::loadCurrentList()
{
    if (!m_Pimpl->filesToLoad.empty())
    {
        fprintf(stdout, "[SceneManager] %zu scene file(s) pending load\n",
            m_Pimpl->filesToLoad.size());
    }
}

const std::vector<std::string>& SceneManager::getSceneFilePaths() const
{
    return m_Pimpl->sceneFilePaths;
}

SceneGraph* SceneManager::getCurrentSceneGraph() const
{
    if (m_Pimpl->currentScene)
        return m_Pimpl->currentScene->GetSceneGraph().get();
    return nullptr;
}

} // namespace caustica

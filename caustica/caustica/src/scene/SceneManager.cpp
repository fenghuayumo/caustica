#include "scene/SceneManager.h"

#include <backend/GpuDevice.h>
#include <assets/loader/TextureLoader.h>
#include <scene/Scene.h>
#include <scene/scene_utils.h>
#include <core/vfs/VFS.h>
#include <core/log.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace
{
    bool LooksLikeInlineSceneJson(const std::string& scene)
    {
        auto it = std::find_if_not(scene.begin(), scene.end(), [](unsigned char ch) {
            return std::isspace(ch);
        });
        return it != scene.end() && *it == '{';
    }
} // namespace


// =============================================================================
// SceneManager implementation
// =============================================================================

SceneManager::SceneManager(caustica::GpuDevice&                     device,
                           caustica::ShaderFactory&                 shaderFactory,
                           std::shared_ptr<caustica::TextureLoader>  textureCache,
                           std::shared_ptr<caustica::SceneTypeFactory> sceneTypeFactory)
    : m_device(device)
    , m_shaderFactory(shaderFactory)
    , m_textureCache(std::move(textureCache))
    , m_sceneTypeFactory(sceneTypeFactory ? std::move(sceneTypeFactory)
                                          : std::make_shared<caustica::SceneTypeFactory>())
{
    m_loader.setLoadFunc([this](std::shared_ptr<caustica::IFileSystem> fs,
                                const std::filesystem::path& path)
    {
        // Import into pending only — never publish m_scene from the worker thread.
        m_pendingScene = loadSceneToPending(std::move(fs), path);
        return m_pendingScene != nullptr;
    });
}

SceneManager::~SceneManager() = default;

// --- Scene discovery ---

void SceneManager::discoverAvailableScenes(const std::filesystem::path& assetsPath)
{
    const std::string mediaExt = ".scene.json";
    const std::string jsonExt = ".json";

    m_sceneFilesAvailable.clear();

    if (!std::filesystem::exists(assetsPath) || !std::filesystem::is_directory(assetsPath))
        return;

    for (const auto& file : std::filesystem::directory_iterator(assetsPath))
    {
        if (!file.is_regular_file()) continue;
        std::string fileName = file.path().filename().string();
        std::string longExt = (fileName.size() <= mediaExt.length())
            ? "" : fileName.substr(fileName.length() - mediaExt.length());
        std::string shortExt = (fileName.size() <= jsonExt.length())
            ? "" : fileName.substr(fileName.length() - jsonExt.length());
        if (longExt == mediaExt || shortExt == jsonExt)
            m_sceneFilesAvailable.push_back(file.path().filename().string());
    }
}

// --- Scene path resolution ---

SceneManager::ResolvedScenePath SceneManager::resolveScenePath(
    const std::string&              sceneName,
    const std::filesystem::path&    assetsPath)
{
    ResolvedScenePath result;
    if (sceneName.empty())
        return result;

    const bool inlineScene = LooksLikeInlineSceneJson(sceneName);
    result.inlineScene = inlineScene;
    if (inlineScene)
    {
        result.inlineJson = sceneName;
        result.path = std::filesystem::path(caustica::inlineSceneSentinel());
        return result;
    }

    std::filesystem::path scenePath(sceneName);
    if (!scenePath.is_absolute() && !std::filesystem::exists(scenePath))
        scenePath = assetsPath / scenePath;
    result.path = scenePath;
    return result;
}

void SceneManager::onSceneLoadedGpuPrep(caustica::Scene& scene, bool& accelRebuildRequested)
{
    accelRebuildRequested = true;

    if (auto* entityWorld = scene.getEntityWorld())
        entityWorld->applyAnimations(0.0f);
}

// --- Active scene state ---

void SceneManager::clearScene()
{
    m_scene.reset();
    m_pendingScene.reset();
}

void SceneManager::retargetCurrentScene(
    const std::string& sceneName,
    const std::filesystem::path& scenePath)
{
    m_currentSceneName = sceneName;
    m_currentScenePath = scenePath;
    m_inlineSceneJson.clear();
}

bool SceneManager::beginSceneSwitch(const std::string&           sceneName,
                                    const std::filesystem::path& assetsPath,
                                    bool                         forceReload)
{
    if (m_currentSceneName == sceneName && !forceReload)
        return false;

    m_currentSceneName = sceneName;

    const ResolvedScenePath resolved = resolveScenePath(sceneName, assetsPath);
    if (resolved.inlineScene)
        m_inlineSceneJson = resolved.inlineJson;
    else
        m_inlineSceneJson.clear();

    m_currentScenePath = resolved.path;
    return true;
}

std::shared_ptr<caustica::Scene> SceneManager::loadSceneToPending(
    std::shared_ptr<caustica::IFileSystem> fs,
    const std::filesystem::path&           sceneFileName)
{
    auto scene = std::make_shared<caustica::Scene>(
        m_device.getDevice(),
        m_shaderFactory,
        std::move(fs),
        m_textureCache,
        m_sceneTypeFactory);

    if (caustica::isInlineScenePath(sceneFileName))
    {
        if (scene->loadFromJsonString(m_inlineSceneJson))
        {
            scene->processNodesRecursive();
            return scene;
        }
        return nullptr;
    }

    if (scene->load(sceneFileName))
    {
        scene->processNodesRecursive();
        return scene;
    }

    return nullptr;
}

std::shared_ptr<caustica::Scene> SceneManager::loadScene(
    std::shared_ptr<caustica::IFileSystem> fs,
    const std::filesystem::path&           sceneFileName)
{
    m_pendingScene = loadSceneToPending(std::move(fs), sceneFileName);
    promotePendingScene();
    return m_scene;
}

void SceneManager::promotePendingScene()
{
    m_scene = std::move(m_pendingScene);
    m_pendingScene.reset();
}

void SceneManager::setAsyncLoadingEnabled(bool enabled)
{
    m_loader.setAsyncEnabled(enabled);
}

void SceneManager::setLoadingCallbacks(std::function<void()> onLoaded,
                                       std::function<void()> onUnloading)
{
    m_loader.onLoaded = [this, onLoaded = std::move(onLoaded)]() {
        promotePendingScene();
        if (onLoaded)
            onLoaded();
    };
    m_loader.onUnloading = [this, onUnloading = std::move(onUnloading)]() {
        if (onUnloading)
            onUnloading();
        m_scene.reset();
        m_pendingScene.reset();
    };
}

void SceneManager::setLoadFailedCallback(std::function<void()> onLoadFailed)
{
    m_onLoadFailed = std::move(onLoadFailed);
}

void SceneManager::beginLoadingScene(std::shared_ptr<caustica::IFileSystem> fs,
                                     const std::filesystem::path& sceneFileName)
{
    // Drain the dedicated render thread only. Device::waitForIdle belongs on the
    // render thread inside onSceneUnloading — never from the logic thread.
    m_device.waitForRenderThreadIdle();

    m_loader.beginLoading(std::move(fs), sceneFileName);

    if (!m_loader.isLoading() && m_loader.isLoaded() && m_loader.onLoaded)
        m_loader.onLoaded();
}

void SceneManager::updateLoading()
{
    const bool wasLoading = m_loader.isLoading();
    m_loader.update();

    if (wasLoading && !m_loader.isLoading() && !m_loader.isLoaded())
    {
        m_pendingScene.reset();
        m_scene.reset();
        if (m_onLoadFailed)
            m_onLoadFailed();
    }
}

bool SceneManager::isSceneLoading() const
{
    return m_loader.isLoading();
}

bool SceneManager::isSceneLoaded() const
{
    return m_loader.isLoaded();
}

bool SceneManager::tryBeginStructureEdit()
{
    if (isSceneLoading())
        return false;
    ++m_structureEditDepth;
    return true;
}

void SceneManager::endStructureEdit()
{
    if (m_structureEditDepth > 0)
        --m_structureEditDepth;
}

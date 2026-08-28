#include "scene/SceneManager.h"

#include <backend/GpuDevice.h>
#include <assets/loader/TextureLoader.h>
#include <scene/Scene.h>
#include <scene/scene_utils.h>
#include <core/path_utils.h>
#include <core/vfs/VFS.h>
#include <core/log.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>

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
    m_sceneFilesAvailable.clear();

    if (!std::filesystem::exists(assetsPath) || !std::filesystem::is_directory(assetsPath))
        return;

    auto isSceneFile = [](const std::filesystem::path& path) {
        const std::string fileName = path.filename().string();
        if (fileName == caustica::c_AssetPackManifest)
            return false;
        if (fileName.size() >= 14 && fileName.compare(fileName.size() - 14, 14, ".material.json") == 0)
            return false;
        if (fileName.size() >= 11 && fileName.compare(fileName.size() - 11, 11, ".scene.json") == 0)
            return true;
        return path.extension() == ".json";
    };

    auto consider = [&](const std::filesystem::path& absolutePath) {
        if (!isSceneFile(absolutePath))
            return;
        std::filesystem::path relative = std::filesystem::relative(absolutePath, assetsPath);
        const std::string relativeGeneric = relative.generic_string();
        if (relative.empty() || relativeGeneric.starts_with(".."))
            relative = absolutePath.filename();
        m_sceneFilesAvailable.push_back(relative.generic_string());
    };

    for (const auto& file : std::filesystem::directory_iterator(assetsPath))
    {
        if (file.is_regular_file())
            consider(file.path());
    }

    const std::filesystem::path scenesDir = assetsPath / caustica::c_ScenesSubFolder;
    std::error_code ec;
    if (std::filesystem::is_directory(scenesDir, ec))
    {
        for (const auto& file : std::filesystem::recursive_directory_iterator(scenesDir, ec))
        {
            if (file.is_regular_file())
                consider(file.path());
        }
    }

    std::sort(m_sceneFilesAvailable.begin(), m_sceneFilesAvailable.end());
    m_sceneFilesAvailable.erase(
        std::unique(m_sceneFilesAvailable.begin(), m_sceneFilesAvailable.end()),
        m_sceneFilesAvailable.end());
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
    if (scenePath.is_absolute())
    {
        result.path = scenePath;
        return result;
    }

    if (std::filesystem::exists(scenePath))
    {
        result.path = std::filesystem::absolute(scenePath);
        return result;
    }

    auto tryExisting = [&](const std::filesystem::path& candidate) -> bool {
        if (candidate.empty() || !std::filesystem::exists(candidate))
            return false;
        result.path = std::filesystem::absolute(candidate);
        return true;
    };

    if (tryExisting(assetsPath / scenePath))
        return result;
    if (tryExisting(assetsPath / caustica::c_ScenesSubFolder / scenePath))
        return result;

    const std::filesystem::path fileName = scenePath.filename();
    std::vector<std::filesystem::path> nameCandidates = { fileName };
    const std::string fileNameStr = fileName.string();
    const bool isSceneJson = fileNameStr.size() >= 11
        && fileNameStr.compare(fileNameStr.size() - 11, 11, ".scene.json") == 0;
    if (fileNameStr == "default.json")
        nameCandidates.emplace_back("default.scene.json");
    else if (!isSceneJson && fileName.extension() == ".json")
        nameCandidates.emplace_back(fileName.stem().string() + ".scene.json");

    const std::filesystem::path scenesDir = assetsPath / caustica::c_ScenesSubFolder;
    std::error_code ec;
    if (std::filesystem::is_directory(scenesDir, ec))
    {
        for (const auto& file : std::filesystem::recursive_directory_iterator(scenesDir, ec))
        {
            if (!file.is_regular_file())
                continue;
            for (const auto& candidateName : nameCandidates)
            {
                if (file.path().filename() == candidateName && tryExisting(file.path()))
                    return result;
            }
        }
    }

    if (tryExisting(assetsPath / fileName))
        return result;

    result.path = assetsPath / scenePath;
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
    // Keep SceneLoader's public loaded state consistent with the scene pointers.
    // Scene switches explicitly tear down the current scene before their deferred
    // import starts. Leaving m_loader loaded made beginLoading() fire onUnloading
    // a second time, racing a duplicate GPU teardown with the new import/frame.
    m_loader.reset();
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
    // Teardown already drained Affinity::Render before prepareForUnload. Do not
    // waitForRenderThreadIdle again here — Import is CPU/IO on LoadSession pipe.
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

#include "engine/SceneManager.h"

#include <backend/GpuDevice.h>
#include <assets/cache/TextureCache.h>
#include <render/Core/RenderSceneTypeFactory.h>
#include <scene/Scene.h>
#include <scene/SceneGraph.h>
#include <core/vfs/VFS.h>
#include <core/log.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

// =============================================================================
// Anonymous helpers
// =============================================================================

namespace
{
    bool LooksLikeInlineSceneJson(const std::string& scene)
    {
        auto it = std::find_if_not(scene.begin(), scene.end(), [](unsigned char ch) {
            return std::isspace(ch);
        });
        return it != scene.end() && *it == '{';
    }

    bool IsEnvironmentMapMediaFile(const std::filesystem::path& path)
    {
        if (!path.has_filename())
            return false;
        const std::string ext = path.extension().string();
        return ext == ".exr" || ext == ".hdr" || ext == ".dds";
    }

    void AppendEnvironmentMapsFromFolder(
        const std::filesystem::path& folder,
        std::vector<std::filesystem::path>& outList)
    {
        if (folder.empty() || !std::filesystem::exists(folder))
            return;

        for (const auto& file : std::filesystem::directory_iterator(folder))
        {
            if (!file.is_regular_file() || !IsEnvironmentMapMediaFile(file.path()))
                continue;

            const std::filesystem::path absolutePath = std::filesystem::absolute(file.path());
            if (std::find(outList.begin(), outList.end(), absolutePath) == outList.end())
                outList.push_back(absolutePath);
        }
    }
} // anonymous namespace


// =============================================================================
// SceneManager implementation
// =============================================================================

SceneManager::SceneManager(caustica::GpuDevice&                     device,
                           caustica::ShaderFactory&                 shaderFactory,
                           std::shared_ptr<caustica::TextureCache>  textureCache,
                           std::shared_ptr<caustica::DescriptorTableManager> descriptorTable)
    : m_device(device)
    , m_shaderFactory(shaderFactory)
    , m_textureCache(std::move(textureCache))
    , m_descriptorTable(std::move(descriptorTable))
{
    m_loader.setLoadFunc([this](std::shared_ptr<caustica::IFileSystem> fs,
                                const std::filesystem::path& path)
    {
        return loadScene(std::move(fs), path) != nullptr;
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
        result.path = std::filesystem::path(SceneManager::inlineSceneSentinel());
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

    for (const auto& anim : scene.GetSceneGraph()->GetAnimations())
        (void)anim->Apply(0.0f);
}

// --- Active scene state ---

void SceneManager::clearScene()
{
    m_scene.reset();
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

std::shared_ptr<caustica::Scene> SceneManager::loadScene(
    std::shared_ptr<caustica::IFileSystem> fs,
    const std::filesystem::path&           sceneFileName)
{
    auto scene = std::make_shared<caustica::Scene>(
        m_device.GetDevice(),
        m_shaderFactory,
        std::move(fs),
        m_textureCache,
        m_descriptorTable,
        std::make_shared<caustica::render::RenderSceneTypeFactory>());

    if (sceneFileName == std::filesystem::path(inlineSceneSentinel()))
    {
        if (scene->LoadFromJsonString(m_inlineSceneJson))
        {
            if (scene->GetSceneGraph() && scene->GetSceneGraph()->GetRootNode())
                scene->ProcessNodesRecursive(scene->GetSceneGraph()->GetRootNode());
            m_scene = scene;
            return scene;
        }
        m_scene.reset();
        return nullptr;
    }

    if (scene->Load(sceneFileName))
    {
        if (scene->GetSceneGraph() && scene->GetSceneGraph()->GetRootNode())
            scene->ProcessNodesRecursive(scene->GetSceneGraph()->GetRootNode());
        m_scene = scene;
        return scene;
    }

    m_scene.reset();
    return nullptr;
}

void SceneManager::setAsyncLoadingEnabled(bool enabled)
{
    m_loader.setAsyncEnabled(enabled);
}

void SceneManager::setLoadingCallbacks(std::function<void()> onLoaded,
                                       std::function<void()> onUnloading)
{
    m_loader.onLoaded = std::move(onLoaded);
    m_loader.onUnloading = std::move(onUnloading);
}

void SceneManager::beginLoadingScene(std::shared_ptr<caustica::IFileSystem> fs,
                                     const std::filesystem::path& sceneFileName)
{
    if (m_textureCache)
        m_textureCache->Reset();

    m_device.GetDevice()->waitForIdle();
    m_device.GetDevice()->runGarbageCollection();

    m_loader.beginLoading(std::move(fs), sceneFileName);

    if (!m_loader.isLoading() && m_loader.isLoaded() && m_loader.onLoaded)
        m_loader.onLoaded();
}

void SceneManager::updateLoading()
{
    m_loader.update();
}

bool SceneManager::isSceneLoading() const
{
    return m_loader.isLoading();
}

bool SceneManager::isSceneLoaded() const
{
    return m_loader.isLoaded();
}

// --- Environment map listing ---

void SceneManager::refreshEnvironmentMapMediaList(
    const std::filesystem::path&              assetsPath,
    const std::filesystem::path&              envMapSubFolder,
    const std::filesystem::path&              currentScenePath,
    std::vector<std::filesystem::path>&       outMediaList,
    std::filesystem::path&                    outMediaFolder)
{
    outMediaList.clear();

    std::filesystem::path sceneDirectory;
    if (!currentScenePath.empty() && currentScenePath != std::filesystem::path(SceneManager::inlineSceneSentinel()))
        sceneDirectory = currentScenePath.parent_path();

    const std::filesystem::path sceneEnvFolder = sceneDirectory / envMapSubFolder;
    const std::filesystem::path assetsEnvFolder = assetsPath / envMapSubFolder;

    AppendEnvironmentMapsFromFolder(assetsEnvFolder, outMediaList);
    AppendEnvironmentMapsFromFolder(sceneEnvFolder, outMediaList);

    if (std::filesystem::exists(assetsEnvFolder))
        outMediaFolder = assetsEnvFolder;
    else
        outMediaFolder = sceneEnvFolder;
}

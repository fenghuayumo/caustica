#include "engine/SceneManager.h"

#include <backend/GpuDevice.h>
#include <assets/cache/TextureCache.h>
#include <scene/Scene.h>
#include <scene/SceneGraph.h>
#include <core/log.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

// =============================================================================
// Anonymous helpers
// =============================================================================

namespace
{
    constexpr const char* c_InlineSceneSentinel = "__CAUSTICA_INLINE_SCENE_JSON__";

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
    if (!currentScenePath.empty() && currentScenePath != std::filesystem::path(c_InlineSceneSentinel))
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

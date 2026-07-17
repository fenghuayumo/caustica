#include "scene/scene_utils.h"
#include <scene/Scene.h>
#include <scene/SceneLightAccess.h>

#include <algorithm>
#include <cctype>
#include <deque>

namespace caustica
{

namespace
{
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
} // namespace

std::string findPreferredScene(const std::vector<std::string>& available,
    const std::string& preferred)
{
    if (available.empty())
        return "";

    for (const auto& s : available)
        if (s.find(preferred) != std::string::npos)
            return s;

    return available.front();
}

ecs::Entity findEnvironmentLightEntity(const Scene& scene)
{
    const scene::SceneEntityWorld* entityWorld = scene.getEntityWorld();
    if (!entityWorld)
        return ecs::NullEntity;

    return scene::findEnvironmentLightEntity(entityWorld->world(), scene.getLightEntities());
}

bool isDirectMeshSceneFile(const std::filesystem::path& sceneFileName)
{
    std::string ext = sceneFileName.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".gltf" || ext == ".glb" || ext == ".obj"
        || ext == ".usd" || ext == ".usda" || ext == ".usdc";
}

void refreshEnvironmentMapMediaList(
    const std::filesystem::path& assetsPath,
    const std::filesystem::path& envMapSubFolder,
    const std::filesystem::path& currentScenePath,
    std::vector<std::filesystem::path>& outMediaList,
    std::filesystem::path& outMediaFolder)
{
    outMediaList.clear();

    std::filesystem::path sceneDirectory;
    if (!currentScenePath.empty() && !isInlineScenePath(currentScenePath))
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

} // namespace caustica

#include "scene/scene_utils.h"
#include <scene/Scene.h>
#include <scene/SceneLightAccess.h>
#include <scene/SceneObjects.h>

#include <algorithm>
#include <cctype>
#include <deque>

namespace caustica
{

std::string FindPreferredScene(const std::vector<std::string>& available,
    const std::string& preferred)
{
    if (available.empty())
        return "";

    for (const auto& s : available)
        if (s.find(preferred) != std::string::npos)
            return s;

    return available.front();
}

ecs::Entity FindEnvironmentLightEntity(const Scene& scene)
{
    const scene::SceneEntityWorld* entityWorld = scene.GetEntityWorld();
    if (!entityWorld)
        return ecs::NullEntity;

    return scene::FindEnvironmentLightEntity(entityWorld->world(), scene.GetLightEntities());
}

bool IsDirectMeshSceneFile(const std::filesystem::path& sceneFileName)
{
    std::string ext = sceneFileName.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".gltf" || ext == ".glb" || ext == ".obj"
        || ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".caususd";
}

} // namespace caustica

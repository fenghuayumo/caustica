#include "scene/scene_utils.h"
#include <core/scene_discovery.h>
#include <core/vfs/VFS.h>
#include "scene/SceneGraph.h"

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

std::shared_ptr<EnvironmentLight> FindEnvironmentLight(
    const std::vector<std::shared_ptr<Light>>& lights)
{
    for (const auto& light : lights)
    {
        if (light->GetLightType() == LightType_Environment)
            return std::dynamic_pointer_cast<EnvironmentLight>(light);
    }
    return nullptr;
}

bool IsDirectMeshSceneFile(const std::filesystem::path& sceneFileName)
{
    std::string ext = sceneFileName.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".gltf" || ext == ".glb" || ext == ".obj";
}

} // namespace caustica

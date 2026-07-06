#pragma once

#include <ecs/Entity.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{

class Light;
class Scene;

bool IsDirectMeshSceneFile(const std::filesystem::path& sceneFileName);

std::string FindPreferredScene(const std::vector<std::string>& available,
    const std::string& preferred);

ecs::Entity FindEnvironmentLightEntity(const Scene& scene);

} // namespace caustica

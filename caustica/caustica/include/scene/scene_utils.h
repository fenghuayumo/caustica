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

bool isDirectMeshSceneFile(const std::filesystem::path& sceneFileName);

std::string findPreferredScene(const std::vector<std::string>& available,
    const std::string& preferred);

ecs::Entity findEnvironmentLightEntity(const Scene& scene);

} // namespace caustica

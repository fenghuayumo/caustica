#pragma once

#include <core/scene_discovery.h>
#include <ecs/Entity.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{

class IFileSystem;
class Light;
class Scene;

// Scene discovery: FindScenes() lives in core/scene_discovery.h (included above).

// Returns true for mesh files that can be loaded directly as a scene.
bool IsDirectMeshSceneFile(const std::filesystem::path& sceneFileName);

// Searches for a given substring in the list of scene paths; returns that
// match if found, otherwise returns the first scene in the list.
std::string FindPreferredScene(const std::vector<std::string>& available,
    const std::string& preferred);

// Finds the first environment light entity in a scene.
// Returns ecs::NullEntity if none found.
ecs::Entity FindEnvironmentLightEntity(const Scene& scene);

} // namespace caustica

#pragma once

#include <core/scene_discovery.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{

class IFileSystem;
class Light;
class EnvironmentLight;

// Scene discovery: FindScenes() lives in core/scene_discovery.h (included above).

// Returns true for mesh files that can be loaded directly as a scene.
bool IsDirectMeshSceneFile(const std::filesystem::path& sceneFileName);

// Searches for a given substring in the list of scene paths; returns that
// match if found, otherwise returns the first scene in the list.
std::string FindPreferredScene(const std::vector<std::string>& available,
    const std::string& preferred);

// Finds the first EnvironmentLight in a list of lights.
// Returns nullptr if none found.
std::shared_ptr<EnvironmentLight> FindEnvironmentLight(
    const std::vector<std::shared_ptr<Light>>& lights);

} // namespace caustica

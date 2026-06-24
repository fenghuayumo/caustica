#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace caustica
{

class IFileSystem;

// Searches 'path' (and direct subdirectories) for scene files
// (.scene.json, .gltf, .glb).
std::vector<std::string> FindScenes(IFileSystem& fs,
    const std::filesystem::path& path);

// Searches for a given substring in the list of scene paths; returns that
// match if found, otherwise returns the first scene in the list.
std::string FindPreferredScene(const std::vector<std::string>& available,
    const std::string& preferred);

} // namespace caustica

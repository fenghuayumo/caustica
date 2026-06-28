#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace caustica
{

class IFileSystem;

// Recursively searches for scene files (.scene.json, .gltf, .glb, .obj).
std::vector<std::string> FindScenes(IFileSystem& fs, const std::filesystem::path& path);

} // namespace caustica

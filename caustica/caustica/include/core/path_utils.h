#pragma once

#include <filesystem>
#include <string>

namespace caustica
{

class IFileSystem;

// --- Executable / runtime directory ---

// Returns the directory containing the current executable.
std::filesystem::path GetDirectoryWithExecutable();

// Override the base path used by GetLocalPath(). Empty means default.
void SetLocalPathBaseOverride(const std::filesystem::path& basePath);

// Returns the current runtime directory (executable dir by default).
std::filesystem::path GetRuntimeDirectory();

// Override the runtime directory. Empty means default.
void SetRuntimeDirectoryOverride(const std::filesystem::path& runtimeDirectory);

// --- Directory search ---

// Searches upward from 'startPath' for a directory 'dirname'.
std::filesystem::path FindDirectory(IFileSystem& fs,
    const std::filesystem::path& startPath,
    const std::filesystem::path& dirname,
    int maxDepth = 5);

// Searches upward from 'startPath' for a file with 'relativeFilePath'.
std::filesystem::path FindDirectoryWithFile(IFileSystem& fs,
    const std::filesystem::path& startPath,
    const std::filesystem::path& relativeFilePath,
    int maxDepth = 5);

// --- Asset / media path resolution ---

// Returns the local path for a "Assets/" subfolder (e.g. "Assets", "Assets/EnvironmentMaps").
std::filesystem::path GetLocalPath(std::string subfolder);

// Resolves a relative media path against a prioritized list of search roots.
// Returns the first existing match, or the first root-joined path as fallback.
std::filesystem::path ResolveMediaRelativePath(
    const std::filesystem::path& localPath,
    std::initializer_list<std::filesystem::path> searchRoots);

// Resolves a scene-relative media path using the standard caustica lookup:
// Assets/ first, then the scene JSON's parent directory.
std::filesystem::path ResolveSceneMediaPath(
    const std::filesystem::path& localPath,
    const std::filesystem::path& sceneDirectory,
    const std::filesystem::path& mediaPath = std::filesystem::path());

// --- Well-known asset folders ---
inline constexpr const char* c_AssetsFolder        = "Assets";
inline constexpr const char* c_EnvMapSubFolder     = "EnvironmentMaps";
inline constexpr const char* c_MaterialsSubFolder  = "Materials";
inline constexpr const char* c_MaterialsExtension  = ".material.json";
inline constexpr const char* c_GameDataSubFolder    = "SampleGame";

// --- Environment map sentinel strings ---
inline constexpr const char* c_EnvMapProcSky            = "==PROCEDURAL_SKY==";
inline constexpr const char* c_EnvMapProcSky_Morning    = "==PROCEDURAL_SKY_MORNING==";
inline constexpr const char* c_EnvMapProcSky_Midday     = "==PROCEDURAL_SKY_MIDDAY==";
inline constexpr const char* c_EnvMapProcSky_Evening    = "==PROCEDURAL_SKY_EVENING==";
inline constexpr const char* c_EnvMapProcSky_Dawn       = "==PROCEDURAL_SKY_DAWN==";
inline constexpr const char* c_EnvMapProcSky_PitchBlack = "==PROCEDURAL_SKY_PITCHBLACK==";
inline constexpr const char* c_EnvMapSceneDefault       = "==SCENE_DEFAULT==";

inline bool IsProceduralSky(const char* str)
{
    if (str == nullptr) return false;
    for (int i = 0; i < 12; i++)
        if (str[i] != c_EnvMapProcSky[i]) return false;
    return true;
}

} // namespace caustica

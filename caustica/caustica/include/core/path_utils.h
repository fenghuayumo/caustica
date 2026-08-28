#pragma once

#include <filesystem>
#include <string>

namespace caustica
{

class IFileSystem;

// --- Executable / runtime directory ---

// Returns the directory containing the current executable.
std::filesystem::path getDirectoryWithExecutable();

// Override the base path used by getLocalPath(). Empty means default.
// This is the directory that *contains* the asset pack folder (usually the
// repo root or the directory next to the executable).
void setLocalPathBaseOverride(const std::filesystem::path& basePath);

// Returns the current runtime directory (executable dir by default).
std::filesystem::path getRuntimeDirectory();

// Override the runtime directory. Empty means default.
void setRuntimeDirectoryOverride(const std::filesystem::path& runtimeDirectory);

// Pin the asset pack root (the directory that contains pack.json / scenes / models).
// Empty clears the pin so discoverAssetPackRoot() runs again.
void setAssetPackRootOverride(const std::filesystem::path& assetPackRoot);

// Resolved asset pack root. Honors (in order): explicit override, --assets /
// EngineAppDesc::assetPackRoot, CAUSTICA_ASSETS_DIR, <resourceRoot>/Assets,
// then assets-builtin/.
std::filesystem::path getAssetPackRoot();

// Locate the asset pack from a runtime directory and resource root.
std::filesystem::path discoverAssetPackRoot(
    const std::filesystem::path& runtimeDirectory,
    const std::filesystem::path& resourceRoot);

// True when dir looks like a caustica asset pack (pack.json or known folders).
bool isAssetPackDirectory(const std::filesystem::path& dir);

// Prefer the canonical pack subfolder, fall back to the Donut/RTXPT name.
std::filesystem::path existingAssetSubfolder(
    const std::filesystem::path& packRoot,
    const char* canonicalName,
    const char* legacyName);

// Models/ -> models/, EnvironmentMaps/ -> env/, Materials/ -> materials/.
std::filesystem::path canonicalAssetRelativePath(const std::filesystem::path& relativePath);
std::filesystem::path legacyAssetRelativePath(const std::filesystem::path& relativePath);

// --- Directory search ---

// Searches upward from 'startPath' for a directory 'dirname'.
std::filesystem::path findDirectory(IFileSystem& fs,
    const std::filesystem::path& startPath,
    const std::filesystem::path& dirname,
    int maxDepth = 5);

// Searches upward from 'startPath' for a file with 'relativeFilePath'.
std::filesystem::path findDirectoryWithFile(IFileSystem& fs,
    const std::filesystem::path& startPath,
    const std::filesystem::path& relativeFilePath,
    int maxDepth = 5);

// --- Asset / media path resolution ---

// Returns a path under the resource root. getLocalPath("Assets") is the pack root.
std::filesystem::path getLocalPath(std::string subfolder);

// Resolves a relative media path against a prioritized list of search roots.
// Returns the first existing match, or the first root-joined path as fallback.
std::filesystem::path resolveMediaRelativePath(
    const std::filesystem::path& localPath,
    std::initializer_list<std::filesystem::path> searchRoots);

// Resolves a scene-relative media path using the standard caustica lookup:
// Assets/ first, then the scene JSON's parent directory.
std::filesystem::path resolveSceneMediaPath(
    const std::filesystem::path& localPath,
    const std::filesystem::path& sceneDirectory,
    const std::filesystem::path& mediaPath = std::filesystem::path());

// --- Well-known asset folders ---
inline constexpr const char* c_AssetsFolder             = "Assets";
inline constexpr const char* c_BuiltinAssetsFolder      = "assets-builtin";
inline constexpr const char* c_AssetPackManifest        = "pack.json";
inline constexpr const char* c_ScenesSubFolder          = "scenes";
inline constexpr const char* c_ModelsSubFolder          = "models";
inline constexpr const char* c_ModelsSubFolderLegacy    = "Models";
inline constexpr const char* c_EnvMapSubFolder          = "env";
inline constexpr const char* c_EnvMapSubFolderLegacy    = "EnvironmentMaps";
inline constexpr const char* c_MaterialsSubFolder       = "materials";
inline constexpr const char* c_MaterialsSubFolderLegacy = "Materials";
inline constexpr const char* c_MaterialsExtension       = ".material.json";
inline constexpr const char* c_GameDataSubFolder        = "game";
inline constexpr const char* c_AssetsEnvVar             = "CAUSTICA_ASSETS_DIR";

// --- Environment map sentinel strings ---
inline constexpr const char* c_EnvMapProcSky            = "==PROCEDURAL_SKY==";
inline constexpr const char* c_EnvMapProcSky_Morning    = "==PROCEDURAL_SKY_MORNING==";
inline constexpr const char* c_EnvMapProcSky_Midday     = "==PROCEDURAL_SKY_MIDDAY==";
inline constexpr const char* c_EnvMapProcSky_Evening    = "==PROCEDURAL_SKY_EVENING==";
inline constexpr const char* c_EnvMapProcSky_Dawn       = "==PROCEDURAL_SKY_DAWN==";
inline constexpr const char* c_EnvMapProcSky_PitchBlack = "==PROCEDURAL_SKY_PITCHBLACK==";
inline constexpr const char* c_EnvMapSceneDefault       = "==SCENE_DEFAULT==";

inline bool isProceduralSky(const char* str)
{
    if (str == nullptr) return false;
    for (int i = 0; i < 12; i++)
        if (str[i] != c_EnvMapProcSky[i]) return false;
    return true;
}

} // namespace caustica

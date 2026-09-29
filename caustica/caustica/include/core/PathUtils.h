#pragma once

#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

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

// True when pack.json declares "kind": "builtin" (the minimal shipped payload).
bool isBuiltinAssetPack(const std::filesystem::path& dir);

// Walk up from a scene file or directory to the pack that contains it
// (pack.json, or an Assets/ folder). Empty if none is found.
std::filesystem::path findAssetPackContaining(const std::filesystem::path& fileOrDir);

// Media root for pack-relative paths in a scene.json (models/, materials/, env/).
// Prefers the pack that contains the scene file; falls back to getAssetPackRoot().
std::filesystem::path mediaRootForScene(const std::filesystem::path& sceneFileOrDir);

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

// --- Canonical asset-path identity ---
//
// Single canonicalization policy for every subsystem that treats a path as an
// asset identity (AssetRegistry, HotReloadTracker, pack mounts). Previously
// each one used a different rule (raw absolute / lexically_normal /
// case-insensitive), so the same file could hash to different keys.
//
// canonicalAssetPath: absolute (vs process CWD) + lexically normal.
// canonicalAssetKey:  generic ('/') separators as a string, additionally
//                    case-folded on Windows where the filesystem is
//                    case-insensitive. POSIX keeps case.
// Keys are for maps/comparison only; use the path form for file I/O.
struct CanonicalAssetIdentity
{
    std::filesystem::path path;
    std::string key;
};

// Single-pass form for callers that need both representations.
[[nodiscard]] CanonicalAssetIdentity canonicalizeAssetPath(const std::filesystem::path& path);

[[nodiscard]] std::filesystem::path canonicalAssetPath(const std::filesystem::path& path);
[[nodiscard]] std::string canonicalAssetKey(const std::filesystem::path& path);

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
inline constexpr const char* kAssetsFolder             = "Assets";
inline constexpr const char* kBuiltinAssetsFolder      = "assets-builtin";
inline constexpr const char* kAssetPackManifest        = "pack.json";
inline constexpr const char* kScenesSubFolder          = "scenes";
inline constexpr const char* kModelsSubFolder          = "models";
inline constexpr const char* kEnvMapSubFolder          = "env";
inline constexpr const char* kMaterialsSubFolder       = "materials";
inline constexpr const char* kMaterialsExtension       = ".material.json";
inline constexpr const char* kMaterialsExtensionAlt    = ".mat.json";
inline constexpr const char* kPrefabsSubFolder         = "prefabs";
inline constexpr const char* kPrefabExtension          = ".prefab.json";
inline constexpr const char* kGameDataSubFolder        = "game";
inline constexpr const char* kAssetsEnvVar             = "CAUSTICA_ASSETS_DIR";

// --- Environment map sentinel strings ---
inline constexpr const char* kEnvMapProcSky            = "==PROCEDURAL_SKY==";
inline constexpr const char* kEnvMapProcSkyMorning    = "==PROCEDURAL_SKY_MORNING==";
inline constexpr const char* kEnvMapProcSkyMidday     = "==PROCEDURAL_SKY_MIDDAY==";
inline constexpr const char* kEnvMapProcSkyEvening    = "==PROCEDURAL_SKY_EVENING==";
inline constexpr const char* kEnvMapProcSkyDawn       = "==PROCEDURAL_SKY_DAWN==";
inline constexpr const char* kEnvMapProcSkyPitchBlack = "==PROCEDURAL_SKY_PITCHBLACK==";
inline constexpr const char* kEnvMapSceneDefault       = "==SCENE_DEFAULT==";

inline bool pathEndsWithIgnoreCase(std::string_view value, std::string_view suffix)
{
    if (value.size() < suffix.size())
        return false;
    for (size_t i = 0; i < suffix.size(); ++i)
    {
        const auto a = static_cast<unsigned char>(value[value.size() - suffix.size() + i]);
        const auto b = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(a) != std::tolower(b))
            return false;
    }
    return true;
}

inline bool isPrefabAssetPath(std::string_view source)
{
    return pathEndsWithIgnoreCase(source, kPrefabExtension);
}

inline bool isMaterialAssetPath(std::string_view source)
{
    return pathEndsWithIgnoreCase(source, kMaterialsExtension)
        || pathEndsWithIgnoreCase(source, kMaterialsExtensionAlt);
}

inline bool isProceduralSky(const char* str)
{
    if (str == nullptr) return false;
    for (int i = 0; i < 12; i++)
        if (str[i] != kEnvMapProcSky[i]) return false;
    return true;
}

} // namespace caustica

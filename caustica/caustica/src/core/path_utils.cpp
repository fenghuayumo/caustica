#include "core/path_utils.h"
#include "core/vfs/VFS.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#include <cstdio>
#include <climits>
#else
#define PATH_MAX MAX_PATH
#include <windows.h>
#endif

namespace caustica
{

std::filesystem::path getDirectoryWithExecutable()
{
    char path[PATH_MAX] = { 0 };
#ifdef _WIN32
    if (GetModuleFileNameA(nullptr, path, sizeof(path)) == 0)
        return "";
#else
    if (readlink("/proc/self/exe", path, sizeof(path)) <= 0)
    {
        if (!getcwd(path, sizeof(path)))
            return "";
    }
#endif
    std::filesystem::path result = path;
    return result.parent_path();
}

std::filesystem::path findDirectory(IFileSystem& fs,
    const std::filesystem::path& startPath,
    const std::filesystem::path& dirname,
    int maxDepth)
{
    std::filesystem::path searchPath;

    for (int depth = 0; depth < maxDepth; depth++)
    {
        std::filesystem::path currentPath = startPath / searchPath / dirname;
        if (fs.folderExists(currentPath))
            return currentPath.lexically_normal();

        searchPath = ".." / searchPath;
    }
    return {};
}

std::filesystem::path findDirectoryWithFile(IFileSystem& fs,
    const std::filesystem::path& startPath,
    const std::filesystem::path& relativeFilePath,
    int maxDepth)
{
    std::filesystem::path searchPath;

    for (int depth = 0; depth < maxDepth; depth++)
    {
        std::filesystem::path currentPath = startPath / searchPath / relativeFilePath;
        if (fs.fileExists(currentPath))
            return currentPath.parent_path().lexically_normal();

        searchPath = ".." / searchPath;
    }
    return {};
}

// --- Asset / media path resolution ---

namespace
{
    std::mutex g_localPathBaseMutex;
    std::filesystem::path g_localPathBaseOverride;
    std::filesystem::path g_runtimeDirectoryOverride;
    std::filesystem::path g_assetPackRootOverride;

    std::filesystem::path GetLocalPathBaseOverride()
    {
        std::lock_guard guard(g_localPathBaseMutex);
        return g_localPathBaseOverride;
    }

    std::filesystem::path GetAssetPackRootOverride()
    {
        std::lock_guard guard(g_localPathBaseMutex);
        return g_assetPackRootOverride;
    }

    std::filesystem::path EnvAssetPackRoot()
    {
        const char* value = std::getenv(c_AssetsEnvVar);
        if (value == nullptr || value[0] == '\0')
            return {};
        std::error_code ec;
        std::filesystem::path path = std::filesystem::absolute(value, ec);
        if (ec)
            return {};
        return path.lexically_normal();
    }

}

bool isAssetPackDirectory(const std::filesystem::path& dir)
{
    if (dir.empty())
        return false;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec) || ec)
        return false;
    if (std::filesystem::exists(dir / c_AssetPackManifest, ec))
        return true;
    if (std::filesystem::is_directory(dir / c_ScenesSubFolder, ec))
        return true;
    if (std::filesystem::is_directory(dir / c_ModelsSubFolder, ec))
        return true;
    if (std::filesystem::is_directory(dir / c_MaterialsSubFolder, ec))
        return true;
    if (std::filesystem::is_directory(dir / c_EnvMapSubFolder, ec))
        return true;
    if (std::filesystem::is_directory(dir / c_PrefabsSubFolder, ec))
        return true;
    return false;
}

bool isBuiltinAssetPack(const std::filesystem::path& dir)
{
    const std::filesystem::path manifest = dir / c_AssetPackManifest;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(manifest, ec) || ec)
        return false;

    std::ifstream in(manifest);
    if (!in)
        return false;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::size_t kind = text.find("\"kind\"");
    if (kind == std::string::npos)
        return false;
    const std::size_t colon = text.find(':', kind);
    if (colon == std::string::npos)
        return false;
    const std::size_t builtin = text.find("\"builtin\"", colon);
    if (builtin == std::string::npos)
        return false;
    const std::size_t nextKey = text.find('"', colon + 1);
    return nextKey == builtin;
}

std::filesystem::path findAssetPackContaining(const std::filesystem::path& fileOrDir)
{
    if (fileOrDir.empty())
        return {};

    std::error_code ec;
    std::filesystem::path cursor = fileOrDir;
    if (std::filesystem::is_regular_file(cursor, ec))
        cursor = cursor.parent_path();
    cursor = std::filesystem::absolute(cursor, ec);
    if (ec)
        return {};

    while (!cursor.empty())
    {
        if (isAssetPackDirectory(cursor))
            return cursor.lexically_normal();

        const std::filesystem::path nested = cursor / c_AssetsFolder;
        if (isAssetPackDirectory(nested))
            return std::filesystem::absolute(nested).lexically_normal();

        const std::filesystem::path parent = cursor.parent_path();
        if (parent == cursor)
            break;
        cursor = parent;
    }
    return {};
}

std::filesystem::path discoverAssetPackRoot(
    const std::filesystem::path& runtimeDirectory,
    const std::filesystem::path& resourceRoot)
{
    const std::filesystem::path envPack = EnvAssetPackRoot();
    if (isAssetPackDirectory(envPack))
        return envPack;

    const std::filesystem::path bases[] = {
        resourceRoot,
        runtimeDirectory,
        runtimeDirectory.empty() ? std::filesystem::path() : runtimeDirectory.parent_path(),
        resourceRoot.empty() ? std::filesystem::path() : resourceRoot.parent_path(),
        getDirectoryWithExecutable(),
        getDirectoryWithExecutable().parent_path(),
    };

    // Startup pack only. Once a scene.json is opened, applySceneSwitch retargets
    // this to the pack that contains that file (models/materials are pack-relative).
    for (const std::filesystem::path& base : bases)
    {
        if (base.empty())
            continue;
        const std::filesystem::path pack = base / c_AssetsFolder;
        if (isAssetPackDirectory(pack))
            return std::filesystem::absolute(pack).lexically_normal();
    }

    for (const std::filesystem::path& base : bases)
    {
        if (base.empty())
            continue;
        const std::filesystem::path builtin = base / c_BuiltinAssetsFolder;
        if (isAssetPackDirectory(builtin))
            return std::filesystem::absolute(builtin).lexically_normal();
    }

    if (isAssetPackDirectory(envPack))
        return envPack;

    const std::filesystem::path fallbackParent = !resourceRoot.empty()
        ? resourceRoot
        : (!runtimeDirectory.empty() ? runtimeDirectory : getDirectoryWithExecutable());
    return (fallbackParent / c_AssetsFolder).lexically_normal();
}

std::filesystem::path getAssetPackRoot()
{
    const std::filesystem::path pinned = GetAssetPackRootOverride();
    if (!pinned.empty())
        return pinned;

    const std::filesystem::path envPack = EnvAssetPackRoot();
    if (isAssetPackDirectory(envPack))
        return envPack;

    return discoverAssetPackRoot(getRuntimeDirectory(), GetLocalPathBaseOverride());
}

void setAssetPackRootOverride(const std::filesystem::path& assetPackRoot)
{
    std::lock_guard guard(g_localPathBaseMutex);
    g_assetPackRootOverride = assetPackRoot.empty()
        ? std::filesystem::path()
        : std::filesystem::absolute(assetPackRoot).lexically_normal();
}

std::filesystem::path getLocalPath(std::string subfolder)
{
    if (subfolder == c_AssetsFolder || subfolder == "Assets")
        return getAssetPackRoot();

    const std::filesystem::path baseOverride = GetLocalPathBaseOverride();
    const std::filesystem::path candidateA = baseOverride.empty()
        ? caustica::getDirectoryWithExecutable() / subfolder
        : baseOverride / subfolder;
    const std::filesystem::path candidateB = baseOverride.empty()
        ? caustica::getDirectoryWithExecutable().parent_path() / subfolder
        : baseOverride.parent_path() / subfolder;
    if (std::filesystem::exists(candidateA))
        return candidateA;
    return candidateB;
}

void setLocalPathBaseOverride(const std::filesystem::path& basePath)
{
    std::lock_guard guard(g_localPathBaseMutex);
    g_localPathBaseOverride = basePath.empty()
        ? std::filesystem::path()
        : std::filesystem::absolute(basePath).lexically_normal();
}

std::filesystem::path getRuntimeDirectory()
{
    std::lock_guard guard(g_localPathBaseMutex);
    if (!g_runtimeDirectoryOverride.empty())
        return g_runtimeDirectoryOverride;
    return caustica::getDirectoryWithExecutable();
}

void setRuntimeDirectoryOverride(const std::filesystem::path& runtimeDirectory)
{
    std::lock_guard guard(g_localPathBaseMutex);
    g_runtimeDirectoryOverride = runtimeDirectory.empty()
        ? std::filesystem::path()
        : std::filesystem::absolute(runtimeDirectory).lexically_normal();
}

std::filesystem::path resolveMediaRelativePath(
    const std::filesystem::path& localPath,
    std::initializer_list<std::filesystem::path> searchRoots)
{
    if (localPath.empty())
        return {};

    if (localPath.is_absolute())
        return std::filesystem::absolute(localPath);

    if (std::filesystem::exists(localPath))
        return std::filesystem::absolute(localPath);

    for (const std::filesystem::path& root : searchRoots)
    {
        if (root.empty())
            continue;
        const std::filesystem::path candidate = root / localPath;
        if (std::filesystem::exists(candidate))
            return std::filesystem::absolute(candidate);
    }

    for (const std::filesystem::path& root : searchRoots)
    {
        if (!root.empty())
            return std::filesystem::absolute(root / localPath);
    }

    return std::filesystem::absolute(localPath);
}

std::filesystem::path mediaRootForScene(const std::filesystem::path& sceneFileOrDir)
{
    const std::filesystem::path pack = findAssetPackContaining(sceneFileOrDir);
    if (!pack.empty())
        return pack;
    return getAssetPackRoot();
}

std::filesystem::path resolveSceneMediaPath(
    const std::filesystem::path& localPath,
    const std::filesystem::path& sceneDirectory,
    const std::filesystem::path& mediaPath)
{
    // Pack-relative refs (models/..., materials/...) resolve from the pack that
    // contains the scene.json, not from the exe-adjacent startup Assets/.
    const std::filesystem::path scenePack = findAssetPackContaining(sceneDirectory);
    const std::filesystem::path assetsRoot = !mediaPath.empty()
        ? mediaPath
        : (!scenePack.empty() ? scenePack : getAssetPackRoot());
    return resolveMediaRelativePath(localPath, { scenePack, assetsRoot, sceneDirectory });
}

} // namespace caustica

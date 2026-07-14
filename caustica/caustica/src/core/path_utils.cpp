#include "core/path_utils.h"
#include "core/vfs/VFS.h"
#include <mutex>

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

    std::filesystem::path GetLocalPathBaseOverride()
    {
        std::lock_guard guard(g_localPathBaseMutex);
        return g_localPathBaseOverride;
    }
}

std::filesystem::path getLocalPath(std::string subfolder)
{
    static std::filesystem::path oneChoice;
    {
        std::filesystem::path baseOverride = GetLocalPathBaseOverride();
        std::filesystem::path candidateA = baseOverride.empty()
            ? caustica::getDirectoryWithExecutable() / subfolder
            : baseOverride / subfolder;
        std::filesystem::path candidateB = baseOverride.empty()
            ? caustica::getDirectoryWithExecutable().parent_path() / subfolder
            : baseOverride.parent_path() / subfolder;
        if (std::filesystem::exists(candidateA))
            oneChoice = candidateA;
        else
            oneChoice = candidateB;
    }
    return oneChoice;
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

std::filesystem::path resolveSceneMediaPath(
    const std::filesystem::path& localPath,
    const std::filesystem::path& sceneDirectory,
    const std::filesystem::path& mediaPath)
{
    const std::filesystem::path assetsRoot = mediaPath.empty()
        ? getLocalPath(c_AssetsFolder)
        : mediaPath;
    return resolveMediaRelativePath(localPath, { assetsRoot, sceneDirectory });
}

} // namespace caustica

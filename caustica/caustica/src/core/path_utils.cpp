#include "core/path_utils.h"
#include "core/vfs/VFS.h"

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

std::filesystem::path GetDirectoryWithExecutable()
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

std::filesystem::path FindDirectory(IFileSystem& fs,
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

std::filesystem::path FindDirectoryWithFile(IFileSystem& fs,
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

} // namespace caustica

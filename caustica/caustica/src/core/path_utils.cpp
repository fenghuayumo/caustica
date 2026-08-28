#include "core/path_utils.h"
#include "core/vfs/VFS.h"

#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

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

    std::string FirstPathComponent(const std::filesystem::path& relativePath)
    {
        auto it = relativePath.begin();
        if (it == relativePath.end())
            return {};
        return it->string();
    }

    std::filesystem::path ReplaceFirstPathComponent(
        const std::filesystem::path& relativePath,
        const std::string& replacement)
    {
        std::filesystem::path out = replacement;
        bool first = true;
        for (const auto& part : relativePath)
        {
            if (first)
            {
                first = false;
                continue;
            }
            out /= part;
        }
        return out;
    }

    bool EqualsIgnoreCase(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            const unsigned char ca = static_cast<unsigned char>(a[i]);
            const unsigned char cb = static_cast<unsigned char>(b[i]);
            if (std::tolower(ca) != std::tolower(cb))
                return false;
        }
        return true;
    }

    std::vector<std::filesystem::path> RelativePathAliases(const std::filesystem::path& relativePath)
    {
        std::vector<std::filesystem::path> aliases;
        auto pushUnique = [&](const std::filesystem::path& candidate) {
            if (candidate.empty())
                return;
            for (const auto& existing : aliases)
            {
                if (existing == candidate)
                    return;
            }
            aliases.push_back(candidate);
        };

        pushUnique(relativePath);
        pushUnique(canonicalAssetRelativePath(relativePath));
        pushUnique(legacyAssetRelativePath(relativePath));
        return aliases;
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
    if (std::filesystem::is_directory(dir / c_ModelsSubFolderLegacy, ec))
        return true;
    if (std::filesystem::is_directory(dir / c_MaterialsSubFolder, ec))
        return true;
    if (std::filesystem::is_directory(dir / c_MaterialsSubFolderLegacy, ec))
        return true;
    if (std::filesystem::is_directory(dir / c_EnvMapSubFolder, ec))
        return true;
    if (std::filesystem::is_directory(dir / c_PrefabsSubFolder, ec))
        return true;
    if (std::filesystem::is_directory(dir / c_EnvMapSubFolderLegacy, ec))
        return true;
    return false;
}

std::filesystem::path existingAssetSubfolder(
    const std::filesystem::path& packRoot,
    const char* canonicalName,
    const char* legacyName)
{
    if (packRoot.empty() || canonicalName == nullptr || canonicalName[0] == '\0')
        return {};

    const std::filesystem::path canonical = packRoot / canonicalName;
    std::error_code ec;
    if (std::filesystem::is_directory(canonical, ec) && !ec)
        return canonical;

    if (legacyName != nullptr && legacyName[0] != '\0')
    {
        const std::filesystem::path legacy = packRoot / legacyName;
        if (std::filesystem::is_directory(legacy, ec) && !ec)
            return legacy;
    }

    return canonical;
}

std::filesystem::path canonicalAssetRelativePath(const std::filesystem::path& relativePath)
{
    if (relativePath.empty() || relativePath.is_absolute())
        return relativePath;

    const std::string first = FirstPathComponent(relativePath);
    if (EqualsIgnoreCase(first, c_ModelsSubFolderLegacy) || EqualsIgnoreCase(first, c_ModelsSubFolder))
        return ReplaceFirstPathComponent(relativePath, c_ModelsSubFolder);
    if (EqualsIgnoreCase(first, c_MaterialsSubFolderLegacy) || EqualsIgnoreCase(first, c_MaterialsSubFolder))
        return ReplaceFirstPathComponent(relativePath, c_MaterialsSubFolder);
    if (EqualsIgnoreCase(first, c_EnvMapSubFolderLegacy) || EqualsIgnoreCase(first, c_EnvMapSubFolder))
        return ReplaceFirstPathComponent(relativePath, c_EnvMapSubFolder);
    return relativePath;
}

std::filesystem::path legacyAssetRelativePath(const std::filesystem::path& relativePath)
{
    if (relativePath.empty() || relativePath.is_absolute())
        return relativePath;

    const std::string first = FirstPathComponent(relativePath);
    if (EqualsIgnoreCase(first, c_ModelsSubFolder) || EqualsIgnoreCase(first, c_ModelsSubFolderLegacy))
        return ReplaceFirstPathComponent(relativePath, c_ModelsSubFolderLegacy);
    if (EqualsIgnoreCase(first, c_MaterialsSubFolder) || EqualsIgnoreCase(first, c_MaterialsSubFolderLegacy))
        return ReplaceFirstPathComponent(relativePath, c_MaterialsSubFolderLegacy);
    if (EqualsIgnoreCase(first, c_EnvMapSubFolder) || EqualsIgnoreCase(first, c_EnvMapSubFolderLegacy))
        return ReplaceFirstPathComponent(relativePath, c_EnvMapSubFolderLegacy);
    return relativePath;
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

    const std::vector<std::filesystem::path> aliases = RelativePathAliases(localPath);

    for (const std::filesystem::path& alias : aliases)
    {
        if (std::filesystem::exists(alias))
            return std::filesystem::absolute(alias);
    }

    for (const std::filesystem::path& root : searchRoots)
    {
        if (root.empty())
            continue;
        for (const std::filesystem::path& alias : aliases)
        {
            const std::filesystem::path candidate = root / alias;
            if (std::filesystem::exists(candidate))
                return std::filesystem::absolute(candidate);
        }
    }

    const std::filesystem::path preferred = canonicalAssetRelativePath(localPath);
    for (const std::filesystem::path& root : searchRoots)
    {
        if (!root.empty())
            return std::filesystem::absolute(root / preferred);
    }

    return std::filesystem::absolute(preferred);
}

std::filesystem::path resolveSceneMediaPath(
    const std::filesystem::path& localPath,
    const std::filesystem::path& sceneDirectory,
    const std::filesystem::path& mediaPath)
{
    const std::filesystem::path assetsRoot = mediaPath.empty()
        ? getAssetPackRoot()
        : mediaPath;
    return resolveMediaRelativePath(localPath, { assetsRoot, sceneDirectory });
}

} // namespace caustica

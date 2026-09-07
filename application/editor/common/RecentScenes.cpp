#include "common/RecentScenes.h"

#include <core/path_utils.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <system_error>

namespace caustica::editor
{

namespace
{

constexpr size_t kMaxRecentScenes = 10;

std::vector<std::string> g_recentScenes;
bool g_recentScenesLoaded = false;

std::filesystem::path resolveStoragePath()
{
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA"))
        return std::filesystem::path(appData) / "Caustica" / "recent_scenes.ini";
#endif
    return std::filesystem::current_path() / "caustica_recent_scenes.ini";
}

std::filesystem::path normalizedPath(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, ec);
    if (ec || result.empty())
    {
        result = std::filesystem::absolute(path, ec);
        if (ec)
            result = path;
    }
    return result.lexically_normal();
}

bool sameScenePath(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
{
    if (lhs == rhs)
        return true;

    std::error_code ec;
    if (std::filesystem::exists(lhs, ec) && std::filesystem::exists(rhs, ec))
        return std::filesystem::equivalent(lhs, rhs, ec);

#if defined(_WIN32)
    const std::string left = lhs.generic_string();
    const std::string right = rhs.generic_string();
    if (left.size() != right.size())
        return false;
    return std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a))
            == std::tolower(static_cast<unsigned char>(b));
    });
#else
    return false;
#endif
}

void save(const std::vector<std::string>& scenes)
{
    const std::filesystem::path path = resolveStoragePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::trunc);
    if (!out)
        return;

    for (const std::string& scene : scenes)
        out << scene << '\n';
}

} // namespace

std::vector<std::string> readRecentScenes()
{
    std::vector<std::string> result;

    std::ifstream in(resolveStoragePath());
    if (!in)
        return result;

    std::string line;
    while (std::getline(in, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;

        const std::filesystem::path path = normalizedPath(std::filesystem::path(line));
        const std::string stored = path.generic_string();
        if (std::find(result.begin(), result.end(), stored) != result.end())
            continue;

        result.push_back(stored);
        if (result.size() == kMaxRecentScenes)
            break;
    }

    return result;
}

void ensureRecentScenesLoaded()
{
    if (!g_recentScenesLoaded)
    {
        g_recentScenes = readRecentScenes();
        g_recentScenesLoaded = true;
    }
}

void storeRecentScenes(std::vector<std::string> scenes)
{
    g_recentScenes = std::move(scenes);
    g_recentScenesLoaded = true;
    save(g_recentScenes);
}

const std::vector<std::string>& recentScenes()
{
    ensureRecentScenesLoaded();
    return g_recentScenes;
}

void loadRecentScenes()
{
    ensureRecentScenesLoaded();
}

void addRecentScene(const std::filesystem::path& scenePath)
{
    if (scenePath.empty())
        return;

    ensureRecentScenesLoaded();
    const std::filesystem::path path = normalizedPath(scenePath);
    std::vector<std::string> scenes = g_recentScenes;
    scenes.erase(
        std::remove_if(
            scenes.begin(),
            scenes.end(),
            [&path](const std::string& scene) {
                return sameScenePath(std::filesystem::path(scene), path);
            }),
        scenes.end());
    scenes.insert(scenes.begin(), path.generic_string());

    if (scenes.size() > kMaxRecentScenes)
        scenes.resize(kMaxRecentScenes);

    storeRecentScenes(std::move(scenes));
}

void clearRecentScenes()
{
    storeRecentScenes({});
}

std::string recentSceneDisplayName(const std::filesystem::path& scenePath)
{
    // Lexical relative paths avoid per-frame filesystem work while building the menu.
    const std::filesystem::path relative = scenePath.lexically_relative(caustica::getAssetPackRoot());
    if (!relative.empty())
    {
        const std::string relativeText = relative.generic_string();
        if (relativeText.rfind("../", 0) != 0 && relativeText != "..")
            return relativeText;
    }

    return normalizedPath(scenePath).generic_string();
}

} // namespace caustica::editor

#include "engine/scene_utils.h"
#include "core/vfs/VFS.h"

#include <deque>

namespace caustica
{

std::vector<std::string> FindScenes(IFileSystem& fs,
    const std::filesystem::path& path)
{
    std::vector<std::string> scenes;
    std::vector<std::string> sceneExtensions = { ".scene.json", ".gltf", ".glb" };

    std::deque<std::filesystem::path> searchList;
    searchList.push_back(path);

    while (!searchList.empty())
    {
        std::filesystem::path currentPath = searchList.front();
        searchList.pop_front();

        fs.enumerateFiles(currentPath, sceneExtensions,
            [&scenes, &currentPath](std::string_view name)
            {
                scenes.push_back((currentPath / name).generic_string());
            });

        fs.enumerateDirectories(currentPath,
            [&searchList, &currentPath](std::string_view name)
            {
                if (name != "glTF-Draco")
                    searchList.push_back(currentPath / name);
            });
    }

    return scenes;
}

std::string FindPreferredScene(const std::vector<std::string>& available,
    const std::string& preferred)
{
    if (available.empty())
        return "";

    for (const auto& s : available)
        if (s.find(preferred) != std::string::npos)
            return s;

    return available.front();
}

} // namespace caustica

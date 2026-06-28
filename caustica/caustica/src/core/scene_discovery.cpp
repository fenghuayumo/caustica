#include <core/scene_discovery.h>
#include <core/vfs/VFS.h>

#include <deque>

namespace caustica
{

std::vector<std::string> FindScenes(IFileSystem& fs, const std::filesystem::path& path)
{
    std::vector<std::string> scenes;
    const std::vector<std::string> sceneExtensions = { ".scene.json", ".gltf", ".glb", ".obj" };

    std::deque<std::filesystem::path> searchList;
    searchList.push_back(path);

    while (!searchList.empty())
    {
        const std::filesystem::path currentPath = searchList.front();
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

} // namespace caustica

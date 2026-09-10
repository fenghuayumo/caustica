#pragma once

#include <filesystem>
#include <memory>

namespace caustica
{
    struct SceneImportResult;
    struct SceneLoadingStats;
    class TextureLoader;
    class SceneTypeFactory;

    // Imports a URDF robot as a visual link tree. Movable joints are stored on the
    // robot root (RobotComponent) for engine visual FK. Collision / drive / effort
    // are ignored. Visual meshes: STL (ASCII/binary) and box/cylinder/sphere primitives.
    class UrdfImporter
    {
    protected:
        std::shared_ptr<SceneTypeFactory> m_SceneTypeFactory;

    public:
        explicit UrdfImporter(std::shared_ptr<SceneTypeFactory> sceneTypeFactory);

        bool load(
            const std::filesystem::path& fileName,
            TextureLoader& textureCache,
            SceneLoadingStats& stats,
            bool asyncTextures,
            SceneImportResult& result,
            const std::filesystem::path& sceneDirectory = std::filesystem::path()) const;
    };
}

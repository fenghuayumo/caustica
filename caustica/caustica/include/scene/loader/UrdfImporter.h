#pragma once

#include <filesystem>
#include <memory>

namespace caustica
{
    struct SceneImportResult;
    struct SceneLoadingStats;
    class IFileSystem;
    class TextureLoader;
    class SceneTypeFactory;

    // Imports a URDF robot as a visual link tree. Movable joints are stored on the
    // robot root (RobotComponent) for engine visual FK. Collision / drive / effort
    // are ignored. Visual meshes: STL (ASCII/binary), COLLADA 1.4 (.dae), and
    // box/cylinder/sphere primitives. COLLADA profile_COMMON materials stay on
    // each triangle set. A URDF material color is the fallback for STL, primitives,
    // and COLLADA primitives that do not bind a material.
    class UrdfImporter
    {
    protected:
        std::shared_ptr<IFileSystem> m_fs;
        std::shared_ptr<SceneTypeFactory> m_SceneTypeFactory;

    public:
        UrdfImporter(
            std::shared_ptr<IFileSystem> fs,
            std::shared_ptr<SceneTypeFactory> sceneTypeFactory);

        bool load(
            const std::filesystem::path& fileName,
            TextureLoader& textureCache,
            SceneLoadingStats& stats,
            bool asyncTextures,
            SceneImportResult& result,
            const std::filesystem::path& sceneDirectory = std::filesystem::path()) const;
    };
}

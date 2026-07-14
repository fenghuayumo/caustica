#pragma once

#include <filesystem>
#include <memory>

namespace caustica
{
    struct SceneImportResult;
    struct SceneLoadingStats;
    class TextureLoader;
    class ThreadPool;
    class SceneTypeFactory;

    // Imports a URDF robot as a static scene hierarchy (links + joints at rest pose).
    // Visual meshes: STL (ASCII/binary). Primitive visuals (box/cylinder/sphere) are generated.
    // Collision geometry and joint animation are ignored.
    class UrdfImporter
    {
    protected:
        std::shared_ptr<SceneTypeFactory> m_SceneTypeFactory;

    public:
        explicit UrdfImporter(std::shared_ptr<SceneTypeFactory> sceneTypeFactory);

        bool Load(
            const std::filesystem::path& fileName,
            TextureLoader& textureCache,
            SceneLoadingStats& stats,
            ThreadPool* threadPool,
            SceneImportResult& result,
            const std::filesystem::path& sceneDirectory = std::filesystem::path()) const;
    };
}

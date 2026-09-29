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

    class ObjImporter
    {
    protected:
        std::shared_ptr<IFileSystem> m_fs;
        std::shared_ptr<SceneTypeFactory> m_SceneTypeFactory;

    public:
        ObjImporter(
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

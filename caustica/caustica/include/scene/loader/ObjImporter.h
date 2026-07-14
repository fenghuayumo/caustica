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

    class ObjImporter
    {
    protected:
        std::shared_ptr<SceneTypeFactory> m_SceneTypeFactory;

    public:
        explicit ObjImporter(std::shared_ptr<SceneTypeFactory> sceneTypeFactory);

        bool load(
            const std::filesystem::path& fileName,
            TextureLoader& textureCache,
            SceneLoadingStats& stats,
            ThreadPool* threadPool,
            SceneImportResult& result,
            const std::filesystem::path& sceneDirectory = std::filesystem::path()) const;
    };
}

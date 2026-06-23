#pragma once

#include <memory>
#include <filesystem>

namespace caustica
{
    class IBlob;
    class IFileSystem;
}

namespace caustica
{
    struct SceneImportResult;
    struct SceneLoadingStats;
    class TextureCache;
    class ThreadPool;
    class SceneGraphNode;
    class SceneTypeFactory;
    class SceneGraphAnimation;
}

namespace caustica
{
    class GltfImporter
    {   
    protected:
        std::shared_ptr<caustica::IFileSystem> m_fs;
        std::shared_ptr<SceneTypeFactory> m_SceneTypeFactory;
        
    public:
        explicit GltfImporter(std::shared_ptr<caustica::IFileSystem> fs, std::shared_ptr<SceneTypeFactory> sceneTypeFactory);
        
        bool Load(
            const std::filesystem::path& fileName,
            TextureCache& textureCache,
            SceneLoadingStats& stats,
            ThreadPool* threadPool,
            SceneImportResult& result,
            const std::filesystem::path& sceneDirectory = std::filesystem::path()) const;
    };
}

#pragma once

#include <memory>
#include <filesystem>

namespace caustica
{
    struct SceneImportResult;
    struct SceneLoadingStats;
    class TextureLoader;
    class ThreadPool;
    class SceneTypeFactory;

    // Loads OpenUSD stages (.usd / .usda / .usdc) via the C++ SDK.
    class CausUsdImporter
    {
    protected:
        std::shared_ptr<SceneTypeFactory> m_SceneTypeFactory;

    public:
        explicit CausUsdImporter(std::shared_ptr<SceneTypeFactory> sceneTypeFactory);

        bool load(
            const std::filesystem::path& fileName,
            TextureLoader& textureCache,
            SceneLoadingStats& stats,
            ThreadPool* threadPool,
            SceneImportResult& result,
            const std::filesystem::path& sceneDirectory = std::filesystem::path()) const;
    };
}

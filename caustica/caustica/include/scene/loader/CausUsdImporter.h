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

    // Loads a .caususd cache produced by tools/usd_bake_caustica.py
    // (baked from OpenUSD .usd/.usda/.usdc stages).
    class CausUsdImporter
    {
    protected:
        std::shared_ptr<SceneTypeFactory> m_SceneTypeFactory;

    public:
        explicit CausUsdImporter(std::shared_ptr<SceneTypeFactory> sceneTypeFactory);

        bool Load(
            const std::filesystem::path& fileName,
            TextureLoader& textureCache,
            SceneLoadingStats& stats,
            ThreadPool* threadPool,
            SceneImportResult& result,
            const std::filesystem::path& sceneDirectory = std::filesystem::path()) const;

        // If `usdPath` is .usd/.usda/.usdc, ensure a sibling .caususd exists (bake via Python if needed).
        static std::filesystem::path ResolveCachePath(const std::filesystem::path& usdOrCachePath);
        static bool EnsureCache(
            const std::filesystem::path& usdPath,
            std::filesystem::path& outCachePath,
            std::string* errorMessage = nullptr);
    };
}

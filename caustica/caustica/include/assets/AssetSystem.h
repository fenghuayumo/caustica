#pragma once

#include <assets/AssetId.h>
#include <assets/AssetHandle.h>
#include <assets/AssetRegistry.h>
#include <assets/AssetFileWatcher.h>
#include <assets/AssetCache.h>

#include <core/JobSystem.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

// =============================================================================
// AssetSystem — facade for the entire asset management pipeline.
//
// Usage:
//   AssetSystem::Initialize(device);
//   auto texHandle = AssetSystem::Get().LoadTexture("path/to/tex.dds", true);
//   AssetSystem::Get().Update(frameIndex);
//   AssetSystem::Shutdown();
// =============================================================================

namespace caustica
{

struct TextureData;
class TextureLoader;
class CommonRenderPasses;
class ThreadPool;

class AssetSystem
{
public:
    static AssetSystem& Get();
    static void Initialize(std::shared_ptr<TextureLoader> legacyTextureLoader = nullptr);
    static void Shutdown();

    void Update(uint64_t frameIndex);

    // --- Subsystems ---
    AssetRegistry& GetRegistry() { return m_Registry; }
    const AssetRegistry& GetRegistry() const { return m_Registry; }
    AssetFileWatcher& GetFileWatcher() { return m_FileWatcher; }

    // --- Typed CPU caches (DIVSHOT-style) ---
    AssetCache<TextureData>& GetTextureCache() { return m_TextureCache; }

    // --- Hot reload ---
    void EnableHotReload(bool enable);
    [[nodiscard]] bool IsHotReloadEnabled() const { return m_HotReloadEnabled; }
    void WatchAssetDirectory(const std::filesystem::path& path);

    // --- Texture helpers ---
    AssetId RegisterTexture(const std::filesystem::path& path);
    void CacheTexture(const AssetId& id, std::shared_ptr<TextureData> texture) { m_TextureCache.Insert(id, std::move(texture)); }
    [[nodiscard]] std::shared_ptr<TextureData> FindTexture(const AssetId& id) { return m_TextureCache.Get(id); }
    [[nodiscard]] std::shared_ptr<TextureData> FindTextureByPath(const std::filesystem::path& path);
    void SetTextureMemoryBudget(size_t bytes) { m_TextureMemoryBudget = bytes; }
    void EvictTexturesToBudget();

    [[nodiscard]] bool IsInitialized() const { return m_Initialized; }

private:
    AssetSystem(const AssetSystem&) = delete;
    AssetSystem& operator=(const AssetSystem&) = delete;
public:
    AssetSystem() = default;
    ~AssetSystem() = default;

    AssetRegistry                 m_Registry;
    AssetFileWatcher              m_FileWatcher;
    AssetCache<TextureData>       m_TextureCache;
    std::shared_ptr<TextureLoader> m_LegacyTextureLoader;
    size_t                        m_TextureMemoryBudget = 512 * 1024 * 1024;

    bool m_Initialized = false;
    bool m_HotReloadEnabled = false;
};

} // namespace caustica

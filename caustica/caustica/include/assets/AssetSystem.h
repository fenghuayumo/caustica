#pragma once

#include <assets/AssetId.h>
#include <assets/AssetHandle.h>
#include <assets/AssetRegistry.h>
#include <assets/AssetFileWatcher.h>

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

class TextureCache;
class CommonRenderPasses;
class ThreadPool;

class AssetSystem
{
public:
    // --- Singleton ---
    static AssetSystem& Get();
    static void Initialize(std::shared_ptr<TextureCache> textureCache);
    static void Shutdown();

    // --- Per-frame update (hot-reload polling) ---
    void Update(uint64_t frameIndex);

    // --- Subsystem access ---
    AssetRegistry& GetRegistry() { return m_Registry; }
    const AssetRegistry& GetRegistry() const { return m_Registry; }
    AssetFileWatcher& GetFileWatcher() { return m_FileWatcher; }

    // --- Hot reload ---
    void EnableHotReload(bool enable);
    [[nodiscard]] bool IsHotReloadEnabled() const { return m_HotReloadEnabled; }

    // Watch a directory for asset changes (e.g. the Assets folder).
    void WatchAssetDirectory(const std::filesystem::path& path);

    // --- Asset loading helpers ---
    // Register a loaded texture in the registry.
    AssetId RegisterTexture(const std::filesystem::path& path);

    // --- Query ---
    [[nodiscard]] bool IsInitialized() const { return m_Initialized; }

private:
    AssetSystem(const AssetSystem&) = delete;
    AssetSystem& operator=(const AssetSystem&) = delete;
public:
    AssetSystem() = default;
    ~AssetSystem() = default;

    AssetRegistry        m_Registry;
    AssetFileWatcher     m_FileWatcher;
    std::shared_ptr<TextureCache> m_TextureCache;

    bool m_Initialized = false;
    bool m_HotReloadEnabled = false;
};

} // namespace caustica

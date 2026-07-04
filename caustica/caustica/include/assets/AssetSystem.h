#pragma once

#include <assets/AssetId.h>
#include <assets/AssetHandle.h>
#include <assets/AssetRegistry.h>
#include <assets/AssetFileWatcher.h>
#include <assets/AssetCache.h>
#include <assets/RuntimeMeshLoadTypes.h>

#include <core/JobSystem.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

// =============================================================================
// AssetSystem facade for the entire asset management pipeline.
//// Usage:
//   AssetSystem::Initialize(device, fileSystem, descriptorTable);
//   auto texHandle = AssetSystem::Get().LoadTexture("path/to/tex.dds", true);
//   AssetSystem::Get().Update(frameIndex);
//   AssetSystem::Shutdown();
// =============================================================================

namespace nvrhi { class IDevice; class ICommandList; }

namespace caustica
{

struct TextureData;
struct LoadedTexture;
class TextureLoader;
namespace rhi { class RenderDevice; }
class ThreadPool;
class IFileSystem;
class IDescriptorTableManager;

class AssetSystem
{
public:
    static AssetSystem& Get();
    // Creates and owns the TextureLoader. Upper layers pass the GPU device, the
    // virtual file system, and the bindless descriptor table; they never
    // construct or own a loader directly.
    static void Initialize(
        nvrhi::IDevice* device,
        std::shared_ptr<IFileSystem> fileSystem,
        std::shared_ptr<IDescriptorTableManager> descriptorTable);
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

    // Single entry point for texture loading. Delegates to the owned loader and
    // records results in m_TextureCache, so callers never touch the loader
    // directly. The sync LoadTexture() uploads to the GPU immediately; the
    // Deferred/Async variants queue the upload for ProcessRenderingThreadCommands.
    [[nodiscard]] std::shared_ptr<LoadedTexture> LoadTexture(
        const std::filesystem::path& path,
        bool sRGB,
        rhi::RenderDevice* renderDevice = nullptr,
        nvrhi::ICommandList* commandList = nullptr);
    [[nodiscard]] std::shared_ptr<LoadedTexture> LoadTextureDeferred(
        const std::filesystem::path& path,
        bool sRGB);
    [[nodiscard]] std::shared_ptr<LoadedTexture> LoadTextureAsync(
        const std::filesystem::path& path,
        bool sRGB,
        ThreadPool& threadPool);

    // The owned loader. Upper layers should prefer the LoadTexture*/FindTexture
    // facade above; this is exposed for the import/baker pipelines that still
    // thread a TextureLoader reference.
    [[nodiscard]] std::shared_ptr<TextureLoader> GetTextureLoader() { return m_TextureLoader; }

    // --- Runtime mesh import helpers ---
    AssetId RegisterMesh(const std::filesystem::path& path);
    [[nodiscard]] RuntimeMeshLoadResult LoadRuntimeMeshFile(
        const RuntimeMeshLoadParams& params,
        const std::filesystem::path& path);
    [[nodiscard]] RuntimeMeshLoadResult LoadRuntimeGltfMeshFile(
        const RuntimeMeshLoadParams& params,
        const std::filesystem::path& path);
    [[nodiscard]] RuntimeMeshLoadResult LoadRuntimeObjMeshFile(
        const RuntimeMeshLoadParams& params,
        const std::filesystem::path& path);

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
    std::shared_ptr<TextureLoader> m_TextureLoader;
    size_t                        m_TextureMemoryBudget = 512 * 1024 * 1024;

    bool m_Initialized = false;
    bool m_HotReloadEnabled = false;
};

} // namespace caustica

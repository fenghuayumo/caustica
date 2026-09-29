#pragma once

#include <assets/AssetId.h>
#include <assets/AssetRegistry.h>
#include <assets/AssetStore.h>
#include <assets/ArtifactCache.h>
#include <assets/DependencyGraph.h>
#include <assets/Handle.h>
#include <assets/HotReload.h>
#include <assets/ImageAsset.h>
#include <assets/TypedAssets.h>

#include <rhi/rhi.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{

class TextureLoader;
class IFileSystem;
class IDescriptorTableManager;
class IBlob;

namespace render { class RenderDevice; }

// Owns texture registry/cache and the TextureLoader that uses them.
// App resource lifecycle: AssetPlugin emplaces + schedules shutdown.
// initialize() is wired by GpuSharedCaches once the GPU descriptor table exists.
class AssetSystem
{
public:
    void initialize(
        caustica::rhi::Device* device,
        std::shared_ptr<IFileSystem> fileSystem,
        std::shared_ptr<IDescriptorTableManager> descriptorTable);
    // Idempotent; safe if never initialized or already shut down.
    void shutdown();

    AssetRegistry& getRegistry() { return m_registry; }
    const AssetRegistry& getRegistry() const { return m_registry; }
    AssetStore<ImageAsset>& images() { return m_images; }
    const AssetStore<ImageAsset>& images() const { return m_images; }
    AssetStore<MeshAsset>& meshes() { return m_meshes; }
    const AssetStore<MeshAsset>& meshes() const { return m_meshes; }
    AssetStore<MaterialAsset>& materials() { return m_materials; }
    const AssetStore<MaterialAsset>& materials() const { return m_materials; }
    AssetStore<SceneAsset>& scenes() { return m_scenes; }
    const AssetStore<SceneAsset>& scenes() const { return m_scenes; }
    AssetStore<ScenePrefabAsset>& prefabs() { return m_prefabs; }
    const AssetStore<ScenePrefabAsset>& prefabs() const { return m_prefabs; }
    DependencyGraph& dependencies() { return m_dependencies; }
    const DependencyGraph& dependencies() const { return m_dependencies; }
    HotReloadTracker& hotReload() { return m_hotReload; }
    const HotReloadTracker& hotReload() const { return m_hotReload; }
    ArtifactCache& artifactCache() { return m_artifactCache; }
    const ArtifactCache& artifactCache() const { return m_artifactCache; }

    [[nodiscard]] std::shared_ptr<TextureLoader> getTextureLoader() { return m_textureLoader; }
    [[nodiscard]] bool isInitialized() const { return m_initialized; }

    Handle<ImageAsset> loadTextureFromFile(
        const std::filesystem::path& path,
        bool sRGB,
        render::RenderDevice* renderDevice,
        caustica::rhi::CommandList* commandList);

    Handle<ImageAsset> loadTextureFromFileDeferred(
        const std::filesystem::path& path,
        bool sRGB);

    Handle<ImageAsset> loadTextureFromFileAsync(
        const std::filesystem::path& path,
        bool sRGB);

    Handle<ImageAsset> loadTextureFromMemory(
        const std::shared_ptr<IBlob>& data,
        const std::string& name,
        const std::string& mimeType,
        bool sRGB,
        render::RenderDevice* renderDevice,
        caustica::rhi::CommandList* commandList);

    Handle<ImageAsset> loadTextureFromMemoryDeferred(
        const std::shared_ptr<IBlob>& data,
        const std::string& name,
        const std::string& mimeType,
        bool sRGB);

    Handle<ImageAsset> loadTextureFromMemoryAsync(
        const std::shared_ptr<IBlob>& data,
        const std::string& name,
        const std::string& mimeType,
        bool sRGB);

    std::shared_ptr<ImageAsset> getLoadedTexture(const std::filesystem::path& path);
    bool unloadTexture(const Handle<ImageAsset>& texture);

    Handle<MeshAsset> registerMeshAsset(
        const std::shared_ptr<MeshInfo>& mesh,
        const std::filesystem::path& sourcePath,
        const std::string& name = {});
    Handle<MaterialAsset> registerMaterialAsset(
        const std::shared_ptr<Material>& material,
        const std::filesystem::path& sourcePath,
        const std::string& name = {});
    Handle<SceneAsset> registerSceneAsset(
        const std::shared_ptr<Scene>& scene,
        const std::filesystem::path& sourcePath,
        const std::string& name = {});
    Handle<ScenePrefabAsset> registerScenePrefab(
        const std::shared_ptr<SceneImportResult>& importResult,
        const std::filesystem::path& sourcePath,
        const std::string& name = {});
    [[nodiscard]] Handle<ScenePrefabAsset> findScenePrefab(const std::filesystem::path& sourcePath) const;
    void clearSceneAssets();

    void addDependency(AssetId asset, AssetId dependency);
    [[nodiscard]] std::vector<HotReloadChange> pollHotReloadChanges();

    bool processRenderingThreadCommands(render::RenderDevice& renderDevice, float timeLimitMilliseconds);
    void loadingFinished();

private:
    AssetRegistry m_registry;
    AssetStore<ImageAsset> m_images;
    AssetStore<MeshAsset> m_meshes;
    AssetStore<MaterialAsset> m_materials;
    AssetStore<SceneAsset> m_scenes;
    AssetStore<ScenePrefabAsset> m_prefabs;
    DependencyGraph m_dependencies;
    HotReloadTracker m_hotReload;
    ArtifactCache m_artifactCache;
    std::shared_ptr<TextureLoader> m_textureLoader;
    bool m_initialized = false;
};

} // namespace caustica

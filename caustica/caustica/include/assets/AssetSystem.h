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

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nvrhi { class IDevice; class ICommandList; }

namespace caustica
{

class TextureLoader;
class IFileSystem;
class IDescriptorTableManager;
class IBlob;
class ThreadPool;

namespace render { class RenderDevice; }

// Owns texture registry/cache and the TextureLoader that uses them.
class AssetSystem
{
public:
    void initialize(
        nvrhi::IDevice* device,
        std::shared_ptr<IFileSystem> fileSystem,
        std::shared_ptr<IDescriptorTableManager> descriptorTable);
    void shutdown();

    AssetRegistry& getRegistry() { return m_Registry; }
    const AssetRegistry& getRegistry() const { return m_Registry; }
    AssetStore<ImageAsset>& images() { return m_Images; }
    const AssetStore<ImageAsset>& images() const { return m_Images; }
    AssetStore<MeshAsset>& meshes() { return m_Meshes; }
    const AssetStore<MeshAsset>& meshes() const { return m_Meshes; }
    AssetStore<MaterialAsset>& materials() { return m_Materials; }
    const AssetStore<MaterialAsset>& materials() const { return m_Materials; }
    AssetStore<SceneAsset>& scenes() { return m_Scenes; }
    const AssetStore<SceneAsset>& scenes() const { return m_Scenes; }
    AssetStore<ScenePrefabAsset>& prefabs() { return m_Prefabs; }
    const AssetStore<ScenePrefabAsset>& prefabs() const { return m_Prefabs; }
    DependencyGraph& dependencies() { return m_Dependencies; }
    const DependencyGraph& dependencies() const { return m_Dependencies; }
    HotReloadTracker& hotReload() { return m_HotReload; }
    const HotReloadTracker& hotReload() const { return m_HotReload; }
    ArtifactCache& artifactCache() { return m_ArtifactCache; }
    const ArtifactCache& artifactCache() const { return m_ArtifactCache; }

    [[nodiscard]] std::shared_ptr<TextureLoader> getTextureLoader() { return m_TextureLoader; }
    [[nodiscard]] bool isInitialized() const { return m_Initialized; }

    Handle<ImageAsset> loadTextureFromFile(
        const std::filesystem::path& path,
        bool sRGB,
        render::RenderDevice* renderDevice,
        nvrhi::ICommandList* commandList);

    Handle<ImageAsset> loadTextureFromFileDeferred(
        const std::filesystem::path& path,
        bool sRGB);

    Handle<ImageAsset> loadTextureFromFileAsync(
        const std::filesystem::path& path,
        bool sRGB,
        ThreadPool& threadPool);

    Handle<ImageAsset> loadTextureFromMemory(
        const std::shared_ptr<IBlob>& data,
        const std::string& name,
        const std::string& mimeType,
        bool sRGB,
        render::RenderDevice* renderDevice,
        nvrhi::ICommandList* commandList);

    Handle<ImageAsset> loadTextureFromMemoryDeferred(
        const std::shared_ptr<IBlob>& data,
        const std::string& name,
        const std::string& mimeType,
        bool sRGB);

    Handle<ImageAsset> loadTextureFromMemoryAsync(
        const std::shared_ptr<IBlob>& data,
        const std::string& name,
        const std::string& mimeType,
        bool sRGB,
        ThreadPool& threadPool);

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
    AssetRegistry m_Registry;
    AssetStore<ImageAsset> m_Images;
    AssetStore<MeshAsset> m_Meshes;
    AssetStore<MaterialAsset> m_Materials;
    AssetStore<SceneAsset> m_Scenes;
    AssetStore<ScenePrefabAsset> m_Prefabs;
    DependencyGraph m_Dependencies;
    HotReloadTracker m_HotReload;
    ArtifactCache m_ArtifactCache;
    std::shared_ptr<TextureLoader> m_TextureLoader;
    bool m_Initialized = false;
};

} // namespace caustica

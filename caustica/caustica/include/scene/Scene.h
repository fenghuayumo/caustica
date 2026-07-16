#pragma once

#include <scene/SceneObjects.h>
#include <scene/SceneAnimation.h>
#include <scene/SceneEcs.h>
#include <scene/SceneRenderData.h>
#include <scene/SceneRenderSnapshot.h>
#include <scene/SceneImport.h>
#include <assets/Handle.h>
#include <assets/TypedAssets.h>
#include <backend/IDescriptorTableManager.h>
#include <rhi/nvrhi.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace Json
{
    class Value;
}

namespace caustica
{
    class IFileSystem;
}

struct GeometryData;

namespace caustica
{
    class ShaderFactory;
    class TextureLoader;
    class ThreadPool;
    class GltfImporter;
    class ObjImporter;
    class CausUsdImporter;
    class UrdfImporter;

    namespace render
    {
        struct SceneGpuResources;
    }
    
    class Scene
    {
    protected:
        std::shared_ptr<caustica::IFileSystem> m_fs;
        std::shared_ptr<SceneTypeFactory> m_SceneTypeFactory;
        std::shared_ptr<TextureLoader> m_TextureLoader;
        std::shared_ptr<IDescriptorTableManager> m_DescriptorTable;
        std::unique_ptr<scene::SceneEntityWorld> m_EntityWorld;
        scene::SceneRenderSnapshot m_RenderSnapshot;
        // Logic-thread extract cache (Bevy Extract / UE proxy sync). Triple-buffer slots
        // receive a copy each publish so incremental extract does not depend on slot reuse.
        scene::SceneRenderData m_LogicExtractCache;
        bool m_LogicExtractCacheValid = false;
        std::shared_ptr<GltfImporter> m_GltfImporter;
        std::shared_ptr<ObjImporter> m_ObjImporter;
        std::shared_ptr<CausUsdImporter> m_CausUsdImporter;
        std::shared_ptr<UrdfImporter> m_UrdfImporter;
        std::vector<SceneImportResult> m_Models;

        bool m_SceneTransformsChanged = false;
        bool m_SceneStructureChanged = false;
        // Stays true after a structure publish until SceneGpuUpdater consumes it, so a
        // delete on a no-render frame still forces GPU buffer rebuild on the next render.
        bool m_gpuStructureFlushPending = false;
        // Logic mutated ECS structure; Extract must exclusive-upload meshes/AS before
        // publishing proxies that reference the new graph.
        bool m_pendingGpuStructureSync = false;

        std::atomic<uint32_t> m_gpuReadFrameIndex{UINT32_MAX};

        std::filesystem::path m_textureSearchDirectory;

        std::shared_ptr<render::SceneGpuResources> m_GpuResources;
        Handle<SceneAsset> m_Asset;

        void loadModelAsync(
            uint32_t index,
            const std::filesystem::path& fileName,
            ThreadPool* threadPool);

        virtual bool loadModelFile(
            const std::filesystem::path& fileName,
            ThreadPool* threadPool,
            SceneImportResult& result);

        void loadModels(
            const Json::Value& modelList, 
            const std::filesystem::path& scenePath, 
            ThreadPool* threadPool);

        SceneImportResult loadBuiltinModel(
            const std::string& builtinName);

        bool loadJsonDocument(
            Json::Value documentRoot,
            const std::filesystem::path& scenePath,
            ThreadPool* threadPool);

        void loadSceneEntities(const Json::Value& nodeList, ecs::Entity parent);
        void loadAnimations(const Json::Value& nodeList);
        
        virtual bool loadCustomData(Json::Value& rootNode, ThreadPool* threadPool);

        std::shared_ptr<class SampleSettings> m_loadedSettings;
        std::shared_ptr<class GameSettings>   m_loadedGameSettings;

        void attachLeafFromJson(ecs::Entity entity, const Json::Value& src);

        [[nodiscard]] const scene::SceneRenderData& getRenderSnapshotForRead() const;

    public:
        virtual ~Scene() = default;

        Scene(
            nvrhi::IDevice* device,
            ShaderFactory& shaderFactory,
            std::shared_ptr<caustica::IFileSystem> fs,
            std::shared_ptr<TextureLoader> textureCache,
            std::shared_ptr<IDescriptorTableManager> descriptorTable,
            std::shared_ptr<SceneTypeFactory> sceneTypeFactory);

        void refreshSceneWorld(uint32_t frameIndex);
        void refreshEntityWorldForFrame(uint32_t frameIndex);
        void publishRenderSnapshot(uint32_t frameIndex);

        // Main/logic thread: extract and publish (ECS refresh runs in App PostUpdate).
        // Optional session inputs resolve ActiveCamera + RenderSettingsSnapshot in the same slot.
        void extractAndPublishRenderSnapshot(
            uint32_t frameIndex, const scene::SessionRenderExtractInputs* session = nullptr);

        // Returns true when extractAndPublishRenderSnapshot already ran for `frameIndex`.
        [[nodiscard]] bool wasRenderSnapshotExtractedOnLogicThread(uint32_t frameIndex) const;

        void beginGpuReadFrame(uint32_t frameIndex);
        void endGpuReadFrame();

        void syncRenderSnapshotGpuIndices(uint32_t frameIndex);
        void acknowledgeGpuStructureConsumed();

        // Bevy-style: mutate ECS only; engine flushes GPU/AS before Extract.
        void requestGpuStructureSync();
        [[nodiscard]] bool needsGpuStructureSync() const { return m_pendingGpuStructureSync; }
        void clearGpuStructureSyncRequest();

        [[nodiscard]] bool hasSceneTransformsChanged() const;
        [[nodiscard]] bool hasSceneStructureChanged() const;
        [[nodiscard]] bool hasSceneTransformsChanged(uint32_t frameIndex) const;
        [[nodiscard]] bool hasSceneStructureChanged(uint32_t frameIndex) const;

        bool load(const std::filesystem::path& jsonFileName);

        virtual bool loadWithThreadPool(const std::filesystem::path& sceneFileName, ThreadPool* threadPool);
        virtual bool loadFromJsonString(const std::string& sceneJson, const std::filesystem::path& scenePath = {});

        static const SceneLoadingStats& getLoadingStats();

        [[nodiscard]] scene::SceneEntityWorld* getEntityWorld() const { return m_EntityWorld.get(); }
        [[nodiscard]] const std::shared_ptr<SceneTypeFactory>& getSceneTypeFactory() const { return m_SceneTypeFactory; }
        [[nodiscard]] dm::box3 getSceneBounds() const;

        [[nodiscard]] const ResourceTracker<Material>& getMaterials() const;
        [[nodiscard]] const ResourceTracker<MeshInfo>& getMeshes() const;
        [[nodiscard]] size_t getGeometryCount() const;
        [[nodiscard]] size_t getMaxGeometryCountPerMesh() const;
        [[nodiscard]] size_t getGeometryInstancesCount() const;

        [[nodiscard]] const std::vector<ecs::Entity>& getMeshInstances() const;
        [[nodiscard]] const std::vector<ecs::Entity>& getSkinnedMeshInstances() const;
        [[nodiscard]] const std::vector<ecs::Entity>& getLightEntities() const;
        [[nodiscard]] const std::vector<ecs::Entity>& getCameraEntities() const;
        [[nodiscard]] const std::vector<ecs::Entity>& getAnimationEntities() const;
        [[nodiscard]] const scene::SceneRenderData& getRenderData() const;

        void attachDirectionalLightToRoot(scene::DirectionalLightComponent component, const std::string& name = {});
        void attachSpotLightToRoot(scene::SpotLightComponent component, const std::string& name = {});
        void attachPointLightToRoot(scene::PointLightComponent component, const std::string& name = {});
        void attachEnvironmentLightToRoot(scene::EnvironmentLightComponent component, const std::string& name = {});
        [[nodiscard]] nvrhi::IDescriptorTable* getDescriptorTable() const { return m_DescriptorTable ? m_DescriptorTable->getDescriptorTable() : nullptr; }
        [[nodiscard]] IDescriptorTableManager* getDescriptorTableManager() const { return m_DescriptorTable.get(); }
        [[nodiscard]] render::SceneGpuResources& getGpuResources() { return *m_GpuResources; }
        [[nodiscard]] const render::SceneGpuResources& getGpuResources() const { return *m_GpuResources; }

        GeometryData* getGeometryData(const MeshGeometry& geometry) const;

        void processNodesRecursive();

        [[nodiscard]] std::shared_ptr<SampleSettings> getSampleSettingsNode() const { return m_loadedSettings; }
        [[nodiscard]] std::shared_ptr<GameSettings>   getGameSettingsNode() const   { return m_loadedGameSettings; }
        [[nodiscard]] const std::vector<SceneImportResult>& getModels() const        { return m_Models; }
        [[nodiscard]] const Handle<SceneAsset>& getAssetHandle() const               { return m_Asset; }
        void setAssetHandle(Handle<SceneAsset> asset)                                { m_Asset = std::move(asset); }

        // Break MeshInfo↔MeshAsset / Material↔MaterialAsset / Scene↔SceneAsset
        // shared_ptr cycles and drop extract-cache retained mesh refs so GPU
        // resources can destroy while the device is still alive (window close).
        void prepareForUnload();

    };
}

#pragma once

#include <scene/SceneObjects.h>
#include <scene/SceneAnimation.h>
#include <scene/SceneEcs.h>
#include <scene/SceneRenderData.h>
#include <scene/SceneRenderSnapshot.h>
#include <scene/SceneRenderCommandQueue.h>
#include <scene/SceneImport.h>
#include <backend/IDescriptorTableManager.h>
#include <rhi/nvrhi.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

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
        scene::SceneRenderCommandQueue m_RenderCommands;
        std::shared_ptr<GltfImporter> m_GltfImporter;
        std::shared_ptr<ObjImporter> m_ObjImporter;
        std::vector<SceneImportResult> m_Models;

        bool m_SceneTransformsChanged = false;
        bool m_SceneStructureChanged = false;

        std::atomic<uint32_t> m_gpuReadFrameIndex{UINT32_MAX};

        std::filesystem::path m_textureSearchDirectory;

        std::shared_ptr<render::SceneGpuResources> m_GpuResources;

        void LoadModelAsync(
            uint32_t index,
            const std::filesystem::path& fileName,
            ThreadPool* threadPool);

        virtual bool LoadModelFile(
            const std::filesystem::path& fileName,
            ThreadPool* threadPool,
            SceneImportResult& result);

        void LoadModels(
            const Json::Value& modelList, 
            const std::filesystem::path& scenePath, 
            ThreadPool* threadPool);

        SceneImportResult LoadBuiltinModel(
            const std::string& builtinName);

        bool LoadJsonDocument(
            Json::Value documentRoot,
            const std::filesystem::path& scenePath,
            ThreadPool* threadPool);

        void LoadSceneEntities(const Json::Value& nodeList, ecs::Entity parent);
        void LoadAnimations(const Json::Value& nodeList);
        
        virtual bool LoadCustomData(Json::Value& rootNode, ThreadPool* threadPool);

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

        void RefreshSceneWorld(uint32_t frameIndex);
        void PublishRenderSnapshot(uint32_t frameIndex);

        // Main/logic thread: refresh ECS (if dirty), extract, and publish for render thread.
        void extractAndPublishRenderSnapshot(uint32_t frameIndex);

        // Returns true when extractAndPublishRenderSnapshot already ran for `frameIndex`.
        [[nodiscard]] bool wasRenderSnapshotExtractedOnLogicThread(uint32_t frameIndex) const;

        void beginGpuReadFrame(uint32_t frameIndex);
        void endGpuReadFrame();

        void syncRenderSnapshotGpuIndices(uint32_t frameIndex);

        [[nodiscard]] bool HasSceneTransformsChanged() const;
        [[nodiscard]] bool HasSceneStructureChanged() const;
        [[nodiscard]] bool HasSceneTransformsChanged(uint32_t frameIndex) const;
        [[nodiscard]] bool HasSceneStructureChanged(uint32_t frameIndex) const;

        bool Load(const std::filesystem::path& jsonFileName);

        virtual bool LoadWithThreadPool(const std::filesystem::path& sceneFileName, ThreadPool* threadPool);
        virtual bool LoadFromJsonString(const std::string& sceneJson, const std::filesystem::path& scenePath = {});

        static const SceneLoadingStats& GetLoadingStats();

        [[nodiscard]] scene::SceneEntityWorld* GetEntityWorld() const { return m_EntityWorld.get(); }
        [[nodiscard]] const std::shared_ptr<SceneTypeFactory>& GetSceneTypeFactory() const { return m_SceneTypeFactory; }
        [[nodiscard]] dm::box3 GetSceneBounds() const;

        [[nodiscard]] const ResourceTracker<Material>& GetMaterials() const;
        [[nodiscard]] const ResourceTracker<MeshInfo>& GetMeshes() const;
        [[nodiscard]] size_t GetGeometryCount() const;
        [[nodiscard]] size_t GetMaxGeometryCountPerMesh() const;
        [[nodiscard]] size_t GetGeometryInstancesCount() const;

        [[nodiscard]] const std::vector<ecs::Entity>& GetMeshInstances() const;
        [[nodiscard]] const std::vector<ecs::Entity>& GetSkinnedMeshInstances() const;
        [[nodiscard]] const std::vector<ecs::Entity>& GetLightEntities() const;
        [[nodiscard]] const std::vector<ecs::Entity>& GetCameraEntities() const;
        [[nodiscard]] const std::vector<ecs::Entity>& GetAnimationEntities() const;
        [[nodiscard]] const scene::SceneRenderData& GetRenderData() const;
        [[nodiscard]] scene::SceneRenderCommandQueue& GetRenderCommands() { return m_RenderCommands; }
        [[nodiscard]] const scene::SceneRenderCommandQueue& GetRenderCommands() const { return m_RenderCommands; }

        void AttachLightToRoot(const std::shared_ptr<Light>& light);
        void AttachLightToRoot(scene::LightComponent component, const std::string& name = {});
        [[nodiscard]] nvrhi::IDescriptorTable* GetDescriptorTable() const { return m_DescriptorTable ? m_DescriptorTable->GetDescriptorTable() : nullptr; }
        [[nodiscard]] IDescriptorTableManager* GetDescriptorTableManager() const { return m_DescriptorTable.get(); }
        [[nodiscard]] render::SceneGpuResources& GetGpuResources() { return *m_GpuResources; }
        [[nodiscard]] const render::SceneGpuResources& GetGpuResources() const { return *m_GpuResources; }

        GeometryData* GetGeometryData(const MeshGeometry& geometry) const;

        void ProcessNodesRecursive();

        [[nodiscard]] std::shared_ptr<SampleSettings> GetSampleSettingsNode() const { return m_loadedSettings; }
        [[nodiscard]] std::shared_ptr<GameSettings>   GetGameSettingsNode() const   { return m_loadedGameSettings; }
        [[nodiscard]] const std::vector<SceneImportResult>& GetModels() const        { return m_Models; }

    };
}

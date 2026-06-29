#pragma once

#include <scene/SceneObjects.h>
#include <scene/SceneAnimation.h>
#include <scene/SceneEcs.h>
#include <scene/SceneImport.h>
#include <backend/IDescriptorTableManager.h>
#include <rhi/nvrhi.h>
#include <vector>
#include <memory>
#include <mutex>
#include <filesystem>
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
    
    class Scene
    {
    protected:
        std::shared_ptr<caustica::IFileSystem> m_fs;
        std::shared_ptr<SceneTypeFactory> m_SceneTypeFactory;
        std::shared_ptr<TextureLoader> m_TextureLoader;
        std::shared_ptr<IDescriptorTableManager> m_DescriptorTable;
        std::unique_ptr<scene::SceneEntityWorld> m_EntityWorld;
        std::shared_ptr<GltfImporter> m_GltfImporter;
        std::shared_ptr<ObjImporter> m_ObjImporter;
        std::vector<SceneImportResult> m_Models;
        bool m_EnableBindlessResources = false;
        bool m_UseResourceDescriptorHeapBindless = false;
        
        nvrhi::BufferHandle m_MaterialBuffer;
        nvrhi::BufferHandle m_GeometryBuffer;
        nvrhi::BufferHandle m_InstanceBuffer;

        nvrhi::DeviceHandle m_Device;
        nvrhi::ShaderHandle m_SkinningShader;
        nvrhi::ComputePipelineHandle m_SkinningPipeline;
        nvrhi::BindingLayoutHandle m_SkinningBindingLayout;

        bool m_RayTracingSupported = false;
        bool m_SceneTransformsChanged = false;
        bool m_SceneStructureChanged = false;

        std::filesystem::path m_textureSearchDirectory;

        struct Resources;
        std::shared_ptr<Resources> m_Resources;

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
        
        void UpdateMaterial(const std::shared_ptr<Material>& material);
        void UpdateGeometry(const std::shared_ptr<MeshInfo>& mesh);
        void UpdateInstance(const std::shared_ptr<MeshInstance>& instance);

        void UpdateSkinnedMeshes(nvrhi::ICommandList* commandList, uint32_t frameIndex);

        void WriteMaterialBuffer(nvrhi::ICommandList* commandList) const;
        void WriteGeometryBuffer(nvrhi::ICommandList* commandList) const;
        void WriteInstanceBuffer(nvrhi::ICommandList* commandList) const;

        virtual void CreateMeshBuffers(nvrhi::ICommandList* commandList);
        virtual nvrhi::BufferHandle CreateMaterialBuffer();
        virtual nvrhi::BufferHandle CreateGeometryBuffer();
        virtual nvrhi::BufferHandle CreateInstanceBuffer();
        virtual nvrhi::BufferHandle CreateMaterialConstantBuffer(const std::string& debugName);

        virtual bool LoadCustomData(Json::Value& rootNode, ThreadPool* threadPool);

        std::shared_ptr<class SampleSettings> m_loadedSettings;
        std::shared_ptr<class GameSettings>   m_loadedGameSettings;

        void attachLeafFromJson(ecs::Entity entity, const Json::Value& src);

    public:
        virtual ~Scene() = default;

        Scene(
            nvrhi::IDevice* device,
            ShaderFactory& shaderFactory,
            std::shared_ptr<caustica::IFileSystem> fs,
            std::shared_ptr<TextureLoader> textureCache,
            std::shared_ptr<IDescriptorTableManager> descriptorTable,
            std::shared_ptr<SceneTypeFactory> sceneTypeFactory);

        void FinishedLoading(uint32_t frameIndex);

        void RefreshSceneWorld(uint32_t frameIndex);
        void RefreshBuffers(nvrhi::ICommandList* commandList, uint32_t frameIndex);
        void Refresh(nvrhi::ICommandList* commandList, uint32_t frameIndex);

        bool Load(const std::filesystem::path& jsonFileName);

        virtual bool LoadWithThreadPool(const std::filesystem::path& sceneFileName, ThreadPool* threadPool);
        virtual bool LoadFromJsonString(const std::string& sceneJson, const std::filesystem::path& scenePath = {});

        static const SceneLoadingStats& GetLoadingStats();

        [[nodiscard]] scene::SceneEntityWorld* GetEntityWorld() const { return m_EntityWorld.get(); }
        [[nodiscard]] dm::box3 GetSceneBounds() const;

        [[nodiscard]] const ResourceTracker<Material>& GetMaterials() const;
        [[nodiscard]] const ResourceTracker<MeshInfo>& GetMeshes() const;
        [[nodiscard]] size_t GetGeometryCount() const;
        [[nodiscard]] size_t GetMaxGeometryCountPerMesh() const;
        [[nodiscard]] size_t GetGeometryInstancesCount() const;

        [[nodiscard]] const std::vector<std::shared_ptr<MeshInstance>>& GetMeshInstances() const;
        [[nodiscard]] const std::vector<std::shared_ptr<SkinnedMeshInstance>>& GetSkinnedMeshInstances() const;
        [[nodiscard]] const std::vector<std::shared_ptr<Light>>& GetLights() const;
        [[nodiscard]] const std::vector<std::shared_ptr<SceneCamera>>& GetCameras() const;
        [[nodiscard]] const std::vector<std::shared_ptr<SceneAnimation>>& GetAnimations() const;

        void AttachLightToRoot(const std::shared_ptr<Light>& light);
        [[nodiscard]] nvrhi::IDescriptorTable* GetDescriptorTable() const { return m_DescriptorTable ? m_DescriptorTable->GetDescriptorTable() : nullptr; }
        [[nodiscard]] nvrhi::IBuffer* GetMaterialBuffer() const { return m_MaterialBuffer; }
        [[nodiscard]] nvrhi::IBuffer* GetGeometryBuffer() const { return m_GeometryBuffer; }
        [[nodiscard]] nvrhi::IBuffer* GetInstanceBuffer() const { return m_InstanceBuffer; }

        GeometryData* GetGeometryData(const MeshGeometry& geometry) const;

        void ProcessNodesRecursive();

        [[nodiscard]] std::shared_ptr<SampleSettings> GetSampleSettingsNode() const { return m_loadedSettings; }
        [[nodiscard]] std::shared_ptr<GameSettings>   GetGameSettingsNode() const   { return m_loadedGameSettings; }
        [[nodiscard]] const std::vector<SceneImportResult>& GetModels() const        { return m_Models; }

    };
}

#pragma once

#include <scene/SceneGraph.h>
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
    struct SceneImportResult;
    class TextureCache;
    class ThreadPool;
    class DescriptorTableManager;
    class GltfImporter;
    class ObjImporter;
    
    class Scene
    {
    protected:
        std::shared_ptr<caustica::IFileSystem> m_fs;
        std::shared_ptr<SceneTypeFactory> m_SceneTypeFactory;
        std::shared_ptr<TextureCache> m_TextureCache;
        std::shared_ptr<DescriptorTableManager> m_DescriptorTable;
        std::shared_ptr<SceneGraph> m_SceneGraph;
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

        // Parent directory of the loaded scene description (scene JSON or glTF file).
        std::filesystem::path m_textureSearchDirectory;

        struct Resources; // Hide the implementation to avoid including <material_cb.h> and <bindless.h> here
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

        void LoadSceneGraph(const Json::Value& nodeList, const std::shared_ptr<SceneGraphNode>& parent);
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

    public:
        virtual ~Scene() = default;

        Scene(
            nvrhi::IDevice* device,
            ShaderFactory& shaderFactory,
            std::shared_ptr<caustica::IFileSystem> fs,
            std::shared_ptr<TextureCache> textureCache,
            std::shared_ptr<DescriptorTableManager> descriptorTable,
            std::shared_ptr<SceneTypeFactory> sceneTypeFactory);

        void FinishedLoading(uint32_t frameIndex);

        // Processes animations, transforms, bounding boxes etc.
        void RefreshSceneGraph(uint32_t frameIndex);

        // Creates missing buffers, uploads vertex buffers, instance data, materials, etc.
        void RefreshBuffers(nvrhi::ICommandList* commandList, uint32_t frameIndex);

        // A combination of RefreshSceneGraph and RefreshBuffers
        void Refresh(nvrhi::ICommandList* commandList, uint32_t frameIndex);

        bool Load(const std::filesystem::path& jsonFileName);

        virtual bool LoadWithThreadPool(const std::filesystem::path& sceneFileName, ThreadPool* threadPool);
        virtual bool LoadFromJsonString(const std::string& sceneJson, const std::filesystem::path& scenePath = {});

        static const SceneLoadingStats& GetLoadingStats();

        [[nodiscard]] std::shared_ptr<SceneGraph> GetSceneGraph() const { return m_SceneGraph; }
        [[nodiscard]] dm::box3 GetSceneBounds() const;
        [[nodiscard]] nvrhi::IDescriptorTable* GetDescriptorTable() const { return m_DescriptorTable ? m_DescriptorTable->GetDescriptorTable() : nullptr; }
        [[nodiscard]] nvrhi::IBuffer* GetMaterialBuffer() const { return m_MaterialBuffer; }
        [[nodiscard]] nvrhi::IBuffer* GetGeometryBuffer() const { return m_GeometryBuffer; }
        [[nodiscard]] nvrhi::IBuffer* GetInstanceBuffer() const { return m_InstanceBuffer; }

        // Can return nullptr if not yet created (by Scene::RefreshBuffers)
        GeometryData* GetGeometryData(const MeshGeometry& geometry) const;

        // Post-load scene graph traversal for light proxy resolution and
        // settings extraction (merged from ExtendedScene::ProcessNodesRecursive).
        void ProcessNodesRecursive(std::shared_ptr<SceneGraphNode> node);

        // --- Accessors merged from ExtendedScene ---
        [[nodiscard]] std::shared_ptr<SampleSettings> GetSampleSettingsNode() const { return m_loadedSettings; }
        [[nodiscard]] std::shared_ptr<GameSettings>   GetGameSettingsNode() const   { return m_loadedGameSettings; }
        [[nodiscard]] const std::vector<SceneImportResult>& GetModels() const        { return m_Models; }

    };
}

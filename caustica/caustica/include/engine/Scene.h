/*
* Copyright (c) 2014-2021, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#pragma once

#include <engine/SceneGraph.h>
#include <nvrhi/nvrhi.h>
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
    
    class Scene
    {
    protected:
        std::shared_ptr<caustica::IFileSystem> m_fs;
        std::shared_ptr<SceneTypeFactory> m_SceneTypeFactory;
        std::shared_ptr<TextureCache> m_TextureCache;
        std::shared_ptr<DescriptorTableManager> m_DescriptorTable;
        std::shared_ptr<SceneGraph> m_SceneGraph;
        std::shared_ptr<GltfImporter> m_GltfImporter;
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

    };
}

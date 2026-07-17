#pragma once

#include <math/math.h>
#include <core/DescriptorHandle.h>
#include <rhi/nvrhi.h>
#include <scene/SceneRenderResourceIds.h>
#include <scene/SceneTypes.h>

// Shared shader structs use unqualified math aliases (float3, uint, float3x4).
using namespace caustica::math;

#include <shaders/bindless.h>
#include <shaders/material_cb.h>

#include <cstdint>
#include <array>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace caustica::render
{

// Per-skinned-mesh GPU state owned by the render side (not ECS).
struct SkinnedMeshGpuState
{
    nvrhi::BufferHandle jointBuffer;
    nvrhi::BindingSetHandle skinningBindingSet;
    bool skinningInitialized = false;
};

struct MeshGpuDebugData
{
    std::shared_ptr<DescriptorHandle> ommArrayDataBufferDescriptor;
    std::shared_ptr<DescriptorHandle> ommDescBufferDescriptor;
    std::shared_ptr<DescriptorHandle> ommIndexBufferDescriptor;
    nvrhi::BufferHandle ommArrayDataBuffer;
    nvrhi::BufferHandle ommDescBuffer;
    nvrhi::BufferHandle ommIndexBuffer;
};

struct MeshGeometryGpuDebugData
{
    uint32_t ommArrayDataOffset = 0xFFFFFFFF;
    uint32_t ommDescBufferOffset = 0xFFFFFFFF;
    uint32_t ommIndexBufferOffset = 0xFFFFFFFF;
    nvrhi::Format ommIndexBufferFormat = nvrhi::Format::R32_UINT;
    uint64_t ommStatsTotalKnown = 0;
    uint64_t ommStatsTotalUnknown = 0;
};

struct MeshGpuRecord
{
    nvrhi::BufferHandle indexBuffer;
    nvrhi::BufferHandle vertexBuffer;
    nvrhi::BufferHandle instanceBuffer;
    std::shared_ptr<DescriptorHandle> indexBufferDescriptor;
    std::shared_ptr<DescriptorHandle> vertexBufferDescriptor;
    std::shared_ptr<DescriptorHandle> instanceBufferDescriptor;
    std::array<nvrhi::BufferRange, size_t(VertexAttribute::Count)> vertexBufferRanges{};
    std::vector<nvrhi::BufferRange> morphTargetBufferRanges;

    nvrhi::rt::AccelStructHandle accelStruct;
    nvrhi::rt::AccelStructHandle accelStructOmm;
    std::vector<nvrhi::rt::OpacityMicromapHandle> opacityMicromaps;
    std::unique_ptr<MeshGpuDebugData> debugData;
    std::vector<MeshGeometryGpuDebugData> geometryDebugData;
    bool debugDataDirty = true;

    [[nodiscard]] bool hasAttribute(VertexAttribute attribute) const
    {
        return vertexBufferRanges[size_t(attribute)].byteSize != 0;
    }

    [[nodiscard]] nvrhi::BufferRange& vertexBufferRange(VertexAttribute attribute)
    {
        return vertexBufferRanges[size_t(attribute)];
    }

    [[nodiscard]] const nvrhi::BufferRange& vertexBufferRange(VertexAttribute attribute) const
    {
        return vertexBufferRanges[size_t(attribute)];
    }
};

struct MaterialGpuRecord
{
    nvrhi::BufferHandle constantsBuffer;
    uint64_t uploadedContentHash = 0;
};

struct MeshGpuUploadCommand
{
    scene::MeshRenderResourceId meshId;
    uint32_t vertexOffset = 0;
    std::vector<dm::float3> positions;
    std::vector<dm::float3> previousPositions;
    std::vector<uint32_t> normals;
    bool recreateVertexBuffer = false;
};

// Per-frame GPU buffer views for the render thread.
// Filled at beginGpuReadFrame; prefer these over Scene::getGpuResources() digs.
struct SceneGpuFrameHandles
{
    nvrhi::BufferHandle instanceBuffer;
    nvrhi::BufferHandle geometryBuffer;

    [[nodiscard]] bool valid() const
    {
        return instanceBuffer != nullptr && geometryBuffer != nullptr;
    }
};

// GPU-side storage derived from a CPU scene.
struct SceneGpuResources
{
    std::vector<MaterialConstants> materialData;
    std::vector<GeometryData> geometryData;
    std::vector<InstanceData> instanceData;

    nvrhi::BufferHandle materialBuffer;
    nvrhi::BufferHandle geometryBuffer;
    nvrhi::BufferHandle instanceBuffer;

    nvrhi::DeviceHandle device;
    nvrhi::ShaderHandle skinningShader;
    nvrhi::ComputePipelineHandle skinningPipeline;
    nvrhi::BindingLayoutHandle skinningBindingLayout;

    // Keyed by ecs::Entity raw id. render thread only.
    std::unordered_map<uint32_t, SkinnedMeshGpuState> skinnedGpuByEntity;
    std::unordered_map<scene::MeshRenderResourceId, MeshGpuRecord,
        scene::MeshRenderResourceId::Hash> meshRegistry;
    std::unordered_map<scene::MaterialRenderResourceId, MaterialGpuRecord,
        scene::MaterialRenderResourceId::Hash> materialRegistry;

    bool enableBindlessResources = false;
    bool useResourceDescriptorHeapBindless = false;
    bool rayTracingSupported = false;

    void enqueueMeshUpload(MeshGpuUploadCommand command)
    {
        std::lock_guard lock(*meshUploadMutex);
        pendingMeshUploads.push_back(std::move(command));
    }

    [[nodiscard]] std::vector<MeshGpuUploadCommand> takePendingMeshUploads()
    {
        std::lock_guard lock(*meshUploadMutex);
        std::vector<MeshGpuUploadCommand> result;
        result.swap(pendingMeshUploads);
        return result;
    }

    void clearSceneResources()
    {
        materialData.clear();
        geometryData.clear();
        instanceData.clear();
        materialBuffer = nullptr;
        geometryBuffer = nullptr;
        instanceBuffer = nullptr;
        skinnedGpuByEntity.clear();
        meshRegistry.clear();
        materialRegistry.clear();
        std::lock_guard lock(*meshUploadMutex);
        pendingMeshUploads.clear();
    }

    [[nodiscard]] SceneGpuFrameHandles frameHandles() const
    {
        return SceneGpuFrameHandles{ instanceBuffer, geometryBuffer };
    }

private:
    std::shared_ptr<std::mutex> meshUploadMutex = std::make_shared<std::mutex>();
    std::vector<MeshGpuUploadCommand> pendingMeshUploads;
};

} // namespace caustica::render

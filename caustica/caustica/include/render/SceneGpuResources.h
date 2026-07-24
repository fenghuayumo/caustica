#pragma once

#include <math/math.h>
#include <core/DescriptorHandle.h>
#include <rhi/rhi.h>
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
    caustica::rhi::BufferHandle jointBuffer;
    caustica::rhi::BindingSetHandle skinningBindingSet;
    bool skinningInitialized = false;
};

struct MeshGpuDebugData
{
    std::shared_ptr<DescriptorHandle> ommArrayDataBufferDescriptor;
    std::shared_ptr<DescriptorHandle> ommDescBufferDescriptor;
    std::shared_ptr<DescriptorHandle> ommIndexBufferDescriptor;
    caustica::rhi::BufferHandle ommArrayDataBuffer;
    caustica::rhi::BufferHandle ommDescBuffer;
    caustica::rhi::BufferHandle ommIndexBuffer;
};

struct MeshGeometryGpuDebugData
{
    uint32_t ommArrayDataOffset = 0xFFFFFFFF;
    uint32_t ommDescBufferOffset = 0xFFFFFFFF;
    uint32_t ommIndexBufferOffset = 0xFFFFFFFF;
    caustica::rhi::Format ommIndexBufferFormat = caustica::rhi::Format::R32_UINT;
    uint64_t ommStatsTotalKnown = 0;
    uint64_t ommStatsTotalUnknown = 0;
};

struct MeshGpuRecord
{
    caustica::rhi::BufferHandle indexBuffer;
    caustica::rhi::BufferHandle vertexBuffer;
    caustica::rhi::BufferHandle instanceBuffer;
    std::shared_ptr<DescriptorHandle> indexBufferDescriptor;
    std::shared_ptr<DescriptorHandle> vertexBufferDescriptor;
    std::shared_ptr<DescriptorHandle> instanceBufferDescriptor;
    std::array<caustica::rhi::BufferRange, size_t(VertexAttribute::Count)> vertexBufferRanges{};
    std::vector<caustica::rhi::BufferRange> morphTargetBufferRanges;

    caustica::rhi::rt::AccelStructHandle accelStruct;
    caustica::rhi::rt::AccelStructHandle accelStructOmm;
    std::vector<caustica::rhi::rt::OpacityMicromapHandle> opacityMicromaps;
    std::unique_ptr<MeshGpuDebugData> debugData;
    std::vector<MeshGeometryGpuDebugData> geometryDebugData;
    bool debugDataDirty = true;

    [[nodiscard]] bool hasAttribute(VertexAttribute attribute) const
    {
        return vertexBufferRanges[size_t(attribute)].byteSize != 0;
    }

    [[nodiscard]] caustica::rhi::BufferRange& vertexBufferRange(VertexAttribute attribute)
    {
        return vertexBufferRanges[size_t(attribute)];
    }

    [[nodiscard]] const caustica::rhi::BufferRange& vertexBufferRange(VertexAttribute attribute) const
    {
        return vertexBufferRanges[size_t(attribute)];
    }
};

struct MaterialGpuRecord
{
    caustica::rhi::BufferHandle constantsBuffer;
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
    caustica::rhi::BufferHandle instanceBuffer;
    caustica::rhi::BufferHandle geometryBuffer;

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

    caustica::rhi::BufferHandle materialBuffer;
    caustica::rhi::BufferHandle geometryBuffer;
    caustica::rhi::BufferHandle instanceBuffer;

    caustica::rhi::DeviceHandle device;
    caustica::rhi::ShaderHandle skinningShader;
    caustica::rhi::ComputePipelineHandle skinningPipeline;
    caustica::rhi::BindingLayoutHandle skinningBindingLayout;

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

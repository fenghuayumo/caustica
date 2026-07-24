#include <render/core/SceneGpuUpdater.h>

#include <render/SceneGpuResources.h>
#include <assets/loader/ShaderFactory.h>
#include <scene/Scene.h>
#include <scene/SceneRenderData.h>
#include <backend/IDescriptorTableManager.h>
#include <core/DescriptorHandle.h>
#include <core/ThreadContext.h>
#include <core/log.h>
#include <rhi/common/misc.h>

#include <cassert>
#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

using namespace caustica::math;
#include <shaders/skinning_cb.h>

#if CAUSTICA_WITH_STATIC_SHADERS
#if CAUSTICA_WITH_DX11
#include "compiled_shaders/skinning_cs.dxbc.h"
#endif
#if CAUSTICA_WITH_DX12
#include "compiled_shaders/skinning_cs.dxil.h"
#endif
#if CAUSTICA_WITH_VULKAN
#include "compiled_shaders/skinning_cs.spirv.h"
#endif
#endif

namespace caustica::render
{

namespace
{

class GpuReadFrameScope
{
public:
    explicit GpuReadFrameScope(Scene& scene, uint32_t frameIndex)
        : m_scene(scene)
    {
        m_scene.beginGpuReadFrame(frameIndex);
    }

    ~GpuReadFrameScope()
    {
        m_scene.endGpuReadFrame();
    }

    GpuReadFrameScope(const GpuReadFrameScope&) = delete;
    GpuReadFrameScope& operator=(const GpuReadFrameScope&) = delete;

private:
    Scene& m_scene;
};

inline void AppendBufferRange(caustica::rhi::BufferRange& range, size_t size, uint64_t& currentBufferSize)
{
    range.byteOffset = currentBufferSize;
    // GPU allocation is 16-byte aligned, but writeBuffer must copy only `size` source
    // bytes — using the padded range.byteSize reads past the end of CPU vectors.
    range.byteSize = size;
    currentBufferSize += caustica::rhi::align(size, size_t(16));
}

template <typename T>
inline void WriteAttributeRange(caustica::rhi::ICommandList* commandList, caustica::rhi::IBuffer* buffer,
    const std::vector<T>& data, const caustica::rhi::BufferRange& range)
{
    if (data.empty() || range.byteSize == 0)
        return;
    assert(range.byteSize == data.size() * sizeof(T));
    commandList->writeBuffer(buffer, data.data(), range.byteSize, range.byteOffset);
}

caustica::rhi::BufferHandle CreateMaterialBuffer(SceneGpuResources& gpu)
{
    caustica::rhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(MaterialConstants) * gpu.materialData.size();
    bufferDesc.debugName = "BindlessMaterials";
    bufferDesc.structStride = sizeof(MaterialConstants);
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.initialState = caustica::rhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;

    return gpu.device->createBuffer(bufferDesc);
}

caustica::rhi::BufferHandle CreateGeometryBuffer(SceneGpuResources& gpu)
{
    caustica::rhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(GeometryData) * gpu.geometryData.size();
    bufferDesc.debugName = "BindlessGeometry";
    bufferDesc.structStride = sizeof(GeometryData);
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.initialState = caustica::rhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;

    return gpu.device->createBuffer(bufferDesc);
}

caustica::rhi::BufferHandle CreateInstanceBuffer(SceneGpuResources& gpu)
{
    const bool needStructuredBuffer = gpu.device->getGraphicsAPI() != caustica::rhi::GraphicsAPI::D3D11;

    caustica::rhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(InstanceData) * gpu.instanceData.size();
    bufferDesc.debugName = "Instances";
    bufferDesc.structStride = needStructuredBuffer ? sizeof(InstanceData) : 0;
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.isVertexBuffer = true;
    bufferDesc.initialState = caustica::rhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;

    return gpu.device->createBuffer(bufferDesc);
}

caustica::rhi::BufferHandle CreateMaterialConstantBuffer(SceneGpuResources& gpu, const std::string& debugName)
{
    caustica::rhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(MaterialConstants);
    bufferDesc.debugName = debugName;
    bufferDesc.isConstantBuffer = true;
    bufferDesc.initialState = caustica::rhi::ResourceStates::ConstantBuffer;
    bufferDesc.keepInitialState = true;

    return gpu.device->createBuffer(bufferDesc);
}

void WriteMaterialBuffer(caustica::rhi::ICommandList* commandList, const SceneGpuResources& gpu)
{
    commandList->writeBuffer(gpu.materialBuffer, gpu.materialData.data(),
        gpu.materialData.size() * sizeof(MaterialConstants));
}

void WriteGeometryBuffer(caustica::rhi::ICommandList* commandList, const SceneGpuResources& gpu)
{
    commandList->writeBuffer(gpu.geometryBuffer, gpu.geometryData.data(),
        gpu.geometryData.size() * sizeof(GeometryData));
}

void WriteInstanceBuffer(caustica::rhi::ICommandList* commandList, const SceneGpuResources& gpu)
{
    commandList->writeBuffer(gpu.instanceBuffer, gpu.instanceData.data(),
        gpu.instanceData.size() * sizeof(InstanceData));
}

uint64_t HashMaterialConstants(const MaterialConstants& constants)
{
    constexpr uint64_t fnvOffset = 14695981039346656037ull;
    constexpr uint64_t fnvPrime = 1099511628211ull;
    uint64_t hash = fnvOffset;
    const auto bytes = std::as_bytes(std::span{ &constants, size_t(1) });
    for (std::byte value : bytes)
    {
        hash ^= std::to_integer<uint8_t>(value);
        hash *= fnvPrime;
    }
    return hash;
}

void UpdateMaterial(
    SceneGpuResources& gpu,
    const scene::MaterialRenderResourceSnapshot& material)
{
    if (material.materialIndex >= gpu.materialData.size())
        return;

    gpu.materialData[material.materialIndex] = gpu.useResourceDescriptorHeapBindless
        ? material.bindlessConstants
        : material.constants;
}

void UpdateGeometry(SceneGpuResources& gpu, const scene::MeshRenderResourceSnapshot& mesh)
{
    if (!mesh.upload)
        return;

    const auto recordIt = gpu.meshRegistry.find(mesh.id);
    if (recordIt == gpu.meshRegistry.end())
        return;
    const MeshGpuRecord& meshGpu = recordIt->second;

    for (const auto& geometry : mesh.geometries)
    {
        if (geometry.globalGeometryIndex < 0
            || static_cast<size_t>(geometry.globalGeometryIndex) >= gpu.geometryData.size())
        {
            caustica::warning("UpdateGeometry: geometry index %u out of range (size=%zu); skipping.",
                geometry.globalGeometryIndex, gpu.geometryData.size());
            continue;
        }

        const uint32_t indexOffset = mesh.indexOffset + geometry.indexOffsetInMesh;
        const uint32_t vertexOffset = mesh.vertexOffset + geometry.vertexOffsetInMesh;

        GeometryData& gdata = gpu.geometryData[geometry.globalGeometryIndex];
        gdata.numIndices = geometry.numIndices;
        gdata.numVertices = geometry.numVertices;
        gdata.indexBufferIndex = meshGpu.indexBufferDescriptor ? meshGpu.indexBufferDescriptor->Get() : -1;
        gdata.indexOffset = indexOffset * sizeof(uint32_t);
        gdata.vertexBufferIndex = meshGpu.vertexBufferDescriptor ? meshGpu.vertexBufferDescriptor->Get() : -1;
        gdata.positionOffset = meshGpu.hasAttribute(VertexAttribute::Position)
            ? uint32_t(vertexOffset * sizeof(float3) + meshGpu.vertexBufferRange(VertexAttribute::Position).byteOffset) : ~0u;
        gdata.prevPositionOffset = meshGpu.hasAttribute(VertexAttribute::PrevPosition)
            ? uint32_t(vertexOffset * sizeof(float3) + meshGpu.vertexBufferRange(VertexAttribute::PrevPosition).byteOffset) : ~0u;
        gdata.texCoord1Offset = meshGpu.hasAttribute(VertexAttribute::TexCoord1)
            ? uint32_t(vertexOffset * sizeof(float2) + meshGpu.vertexBufferRange(VertexAttribute::TexCoord1).byteOffset) : ~0u;
        gdata.texCoord2Offset = meshGpu.hasAttribute(VertexAttribute::TexCoord2)
            ? uint32_t(vertexOffset * sizeof(float2) + meshGpu.vertexBufferRange(VertexAttribute::TexCoord2).byteOffset) : ~0u;
        gdata.normalOffset = meshGpu.hasAttribute(VertexAttribute::Normal)
            ? uint32_t(vertexOffset * sizeof(uint32_t) + meshGpu.vertexBufferRange(VertexAttribute::Normal).byteOffset) : ~0u;
        gdata.tangentOffset = meshGpu.hasAttribute(VertexAttribute::Tangent)
            ? uint32_t(vertexOffset * sizeof(uint32_t) + meshGpu.vertexBufferRange(VertexAttribute::Tangent).byteOffset) : ~0u;
        gdata.curveRadiusOffset = meshGpu.hasAttribute(VertexAttribute::CurveRadius)
            ? uint32_t(vertexOffset * sizeof(float) + meshGpu.vertexBufferRange(VertexAttribute::CurveRadius).byteOffset) : ~0u;
        gdata.materialIndex = geometry.materialIndex;
    }
}

void UpdateInstance(SceneGpuResources& gpu, const scene::MeshInstanceRenderProxy& proxy,
    uint32_t compactedGeometryInstanceIndex)
{
    if (proxy.instanceIndex < 0)
        return;

    if (static_cast<size_t>(proxy.instanceIndex) >= gpu.instanceData.size())
        return;

    InstanceData& idata = gpu.instanceData[proxy.instanceIndex];
    affineToColumnMajor(proxy.transformFloat, idata.transform);
    affineToColumnMajor(proxy.previousTransformFloat, idata.prevTransform);

    // Must match TLAS instanceID / MaterialGpuCache dense prefix — not a possibly
    // stale proxy.geometryInstanceIndex from a mid-import snapshot.
    idata.firstGeometryInstanceIndex = int32_t(compactedGeometryInstanceIndex);
    idata.numGeometries = proxy.geometryCount;
    idata.firstGeometryIndex = proxy.firstGlobalGeometryIndex;
    idata.flags = 0u;

    if (proxy.meshType == MeshType::CurveDisjointOrthogonalTriangleStrips)
    {
        idata.flags |= InstanceFlags_CurveDisjointOrthogonalTriangleStrips;
    }
    else if (proxy.meshType == MeshType::CurveLinearSweptSpheres)
    {
        idata.flags |= InstanceFlags_CurveLinearSweptSpheres;
    }
}

void EnsureMeshGpuBuffers(
    SceneGpuResources& gpu,
    const scene::SceneRenderData& renderData,
    IDescriptorTableManager* descriptorTable,
    caustica::rhi::ICommandList* commandList)
{
    for (const auto& mesh : renderData.meshSnapshots)
    {
        const auto& buffers = mesh.upload;

        if (!buffers)
            continue;

        MeshGpuRecord& meshGpu = gpu.meshRegistry[mesh.id];

        if (!buffers->indexData.empty() && !meshGpu.indexBuffer)
        {
            caustica::rhi::BufferDesc bufferDesc;
            bufferDesc.isIndexBuffer = true;
            bufferDesc.byteSize = buffers->indexData.size() * sizeof(uint32_t);
            bufferDesc.debugName = "IndexBuffer";
            bufferDesc.canHaveTypedViews = true;
            bufferDesc.canHaveRawViews = true;
            bufferDesc.format = caustica::rhi::Format::R32_UINT;
            bufferDesc.isAccelStructBuildInput = gpu.rayTracingSupported;

            meshGpu.indexBuffer = gpu.device->createBuffer(bufferDesc);
            if (!meshGpu.indexBuffer)
            {
                caustica::error("Failed to create index buffer for mesh '%s' (%zu indices).",
                    mesh.debugName.c_str(), buffers->indexData.size());
                continue;
            }

            if (descriptorTable)
            {
                meshGpu.indexBufferDescriptor = std::make_shared<DescriptorHandle>(
                    descriptorTable->createDescriptorHandle(caustica::rhi::BindingSetItem::RawBuffer_SRV(0, meshGpu.indexBuffer)));
            }

            commandList->beginTrackingBufferState(meshGpu.indexBuffer, caustica::rhi::ResourceStates::Common);
            commandList->writeBuffer(meshGpu.indexBuffer, buffers->indexData.data(), buffers->indexData.size() * sizeof(uint32_t));

            caustica::rhi::ResourceStates state = caustica::rhi::ResourceStates::IndexBuffer | caustica::rhi::ResourceStates::ShaderResource;
            if (bufferDesc.isAccelStructBuildInput)
                state = state | caustica::rhi::ResourceStates::AccelStructBuildInput;

            commandList->setPermanentBufferState(meshGpu.indexBuffer, state);
            commandList->commitBarriers();
        }
        if (descriptorTable && meshGpu.indexBuffer && !meshGpu.indexBufferDescriptor)
        {
            meshGpu.indexBufferDescriptor = std::make_shared<DescriptorHandle>(
                descriptorTable->createDescriptorHandle(
                    caustica::rhi::BindingSetItem::RawBuffer_SRV(0, meshGpu.indexBuffer)));
        }

        if (!meshGpu.vertexBuffer)
        {
            caustica::rhi::BufferDesc bufferDesc;
            bufferDesc.isVertexBuffer = true;
            bufferDesc.byteSize = 0;
            bufferDesc.debugName = "VertexBuffer";
            bufferDesc.canHaveTypedViews = true;
            bufferDesc.canHaveRawViews = true;
            bufferDesc.isAccelStructBuildInput = gpu.rayTracingSupported;

            caustica::rhi::ResourceStates state = caustica::rhi::ResourceStates::VertexBuffer | caustica::rhi::ResourceStates::ShaderResource;
            if (bufferDesc.isAccelStructBuildInput)
                state = state | caustica::rhi::ResourceStates::AccelStructBuildInput;
            bufferDesc.initialState = state;
            bufferDesc.keepInitialState = true;

            if (!buffers->positionData.empty())
            {
                AppendBufferRange(meshGpu.vertexBufferRange(VertexAttribute::Position),
                    buffers->positionData.size() * sizeof(buffers->positionData[0]), bufferDesc.byteSize);
                AppendBufferRange(meshGpu.vertexBufferRange(VertexAttribute::PrevPosition),
                    buffers->positionData.size() * sizeof(buffers->positionData[0]), bufferDesc.byteSize);
            }

            if (!buffers->normalData.empty())
                AppendBufferRange(meshGpu.vertexBufferRange(VertexAttribute::Normal), buffers->normalData.size() * sizeof(buffers->normalData[0]), bufferDesc.byteSize);
            if (!buffers->tangentData.empty())
                AppendBufferRange(meshGpu.vertexBufferRange(VertexAttribute::Tangent), buffers->tangentData.size() * sizeof(buffers->tangentData[0]), bufferDesc.byteSize);
            if (!buffers->texcoord1Data.empty())
                AppendBufferRange(meshGpu.vertexBufferRange(VertexAttribute::TexCoord1), buffers->texcoord1Data.size() * sizeof(buffers->texcoord1Data[0]), bufferDesc.byteSize);
            if (!buffers->texcoord2Data.empty())
                AppendBufferRange(meshGpu.vertexBufferRange(VertexAttribute::TexCoord2), buffers->texcoord2Data.size() * sizeof(buffers->texcoord2Data[0]), bufferDesc.byteSize);
            if (!buffers->weightData.empty())
                AppendBufferRange(meshGpu.vertexBufferRange(VertexAttribute::JointWeights), buffers->weightData.size() * sizeof(buffers->weightData[0]), bufferDesc.byteSize);
            if (!buffers->jointData.empty())
                AppendBufferRange(meshGpu.vertexBufferRange(VertexAttribute::JointIndices), buffers->jointData.size() * sizeof(buffers->jointData[0]), bufferDesc.byteSize);
            if (!buffers->radiusData.empty())
                AppendBufferRange(meshGpu.vertexBufferRange(VertexAttribute::CurveRadius), buffers->radiusData.size() * sizeof(buffers->radiusData[0]), bufferDesc.byteSize);

            if (bufferDesc.byteSize == 0)
                continue;

            meshGpu.vertexBuffer = gpu.device->createBuffer(bufferDesc);
            if (!meshGpu.vertexBuffer)
            {
                caustica::error("Failed to create vertex buffer for mesh '%s' (%llu bytes).",
                    mesh.debugName.c_str(),
                    static_cast<unsigned long long>(bufferDesc.byteSize));
                continue;
            }
            if (descriptorTable)
            {
                meshGpu.vertexBufferDescriptor = std::make_shared<DescriptorHandle>(
                    descriptorTable->createDescriptorHandle(caustica::rhi::BindingSetItem::RawBuffer_SRV(0, meshGpu.vertexBuffer)));
            }

            commandList->beginTrackingBufferState(meshGpu.vertexBuffer, caustica::rhi::ResourceStates::Common);

            if (!buffers->positionData.empty())
            {
                WriteAttributeRange(commandList, meshGpu.vertexBuffer, buffers->positionData,
                    meshGpu.vertexBufferRange(VertexAttribute::Position));
                WriteAttributeRange(commandList, meshGpu.vertexBuffer, buffers->positionData,
                    meshGpu.vertexBufferRange(VertexAttribute::PrevPosition));
            }

            WriteAttributeRange(commandList, meshGpu.vertexBuffer, buffers->normalData,
                meshGpu.vertexBufferRange(VertexAttribute::Normal));
            WriteAttributeRange(commandList, meshGpu.vertexBuffer, buffers->tangentData,
                meshGpu.vertexBufferRange(VertexAttribute::Tangent));
            WriteAttributeRange(commandList, meshGpu.vertexBuffer, buffers->texcoord1Data,
                meshGpu.vertexBufferRange(VertexAttribute::TexCoord1));
            WriteAttributeRange(commandList, meshGpu.vertexBuffer, buffers->texcoord2Data,
                meshGpu.vertexBufferRange(VertexAttribute::TexCoord2));
            WriteAttributeRange(commandList, meshGpu.vertexBuffer, buffers->weightData,
                meshGpu.vertexBufferRange(VertexAttribute::JointWeights));
            WriteAttributeRange(commandList, meshGpu.vertexBuffer, buffers->jointData,
                meshGpu.vertexBufferRange(VertexAttribute::JointIndices));
            WriteAttributeRange(commandList, meshGpu.vertexBuffer, buffers->radiusData,
                meshGpu.vertexBufferRange(VertexAttribute::CurveRadius));

            commandList->setBufferState(meshGpu.vertexBuffer, state);
            commandList->commitBarriers();
        }
        if (descriptorTable && meshGpu.vertexBuffer && !meshGpu.vertexBufferDescriptor)
        {
            meshGpu.vertexBufferDescriptor = std::make_shared<DescriptorHandle>(
                descriptorTable->createDescriptorHandle(
                    caustica::rhi::BindingSetItem::RawBuffer_SRV(0, meshGpu.vertexBuffer)));
        }
    }

    auto& skinnedGpuMap = gpu.skinnedGpuByEntity;
    for (const scene::SkinnedMeshRenderProxy& proxy : renderData.skinnedMeshes)
    {
        const scene::MeshRenderResourceSnapshot* skinnedMesh = renderData.findMesh(proxy.meshId);
        const scene::MeshRenderResourceSnapshot* prototypeMesh = renderData.findMesh(proxy.prototypeMeshId);
        if (!skinnedMesh || !prototypeMesh || !proxy.meshId || !proxy.prototypeMeshId)
            continue;

        auto prototypeGpuIt = gpu.meshRegistry.find(proxy.prototypeMeshId);
        if (prototypeGpuIt == gpu.meshRegistry.end())
            continue;
        MeshGpuRecord& prototypeGpu = prototypeGpuIt->second;
        MeshGpuRecord& skinnedGpuMesh = gpu.meshRegistry[proxy.meshId];
        SkinnedMeshGpuState& skinnedGpu = skinnedGpuMap[static_cast<uint32_t>(proxy.entity)];

        if (!skinnedGpuMesh.vertexBuffer)
        {
            const uint32_t totalVertices = skinnedMesh->totalVertices;

            skinnedGpuMesh.indexBuffer = prototypeGpu.indexBuffer;
            skinnedGpuMesh.indexBufferDescriptor = prototypeGpu.indexBufferDescriptor;

            size_t skinnedVertexBufferSize = 0;
            assert(prototypeGpu.hasAttribute(VertexAttribute::Position));

            AppendBufferRange(skinnedGpuMesh.vertexBufferRange(VertexAttribute::Position), totalVertices * sizeof(float3), skinnedVertexBufferSize);
            AppendBufferRange(skinnedGpuMesh.vertexBufferRange(VertexAttribute::PrevPosition), totalVertices * sizeof(float3), skinnedVertexBufferSize);

            if (prototypeGpu.hasAttribute(VertexAttribute::Normal))
                AppendBufferRange(skinnedGpuMesh.vertexBufferRange(VertexAttribute::Normal), totalVertices * sizeof(uint32_t), skinnedVertexBufferSize);
            if (prototypeGpu.hasAttribute(VertexAttribute::Tangent))
                AppendBufferRange(skinnedGpuMesh.vertexBufferRange(VertexAttribute::Tangent), totalVertices * sizeof(uint32_t), skinnedVertexBufferSize);
            if (prototypeGpu.hasAttribute(VertexAttribute::TexCoord1))
                AppendBufferRange(skinnedGpuMesh.vertexBufferRange(VertexAttribute::TexCoord1), totalVertices * sizeof(float2), skinnedVertexBufferSize);
            if (prototypeGpu.hasAttribute(VertexAttribute::TexCoord2))
                AppendBufferRange(skinnedGpuMesh.vertexBufferRange(VertexAttribute::TexCoord2), totalVertices * sizeof(float2), skinnedVertexBufferSize);

            caustica::rhi::BufferDesc bufferDesc;
            bufferDesc.isVertexBuffer = true;
            bufferDesc.byteSize = skinnedVertexBufferSize;
            bufferDesc.debugName = "SkinnedVertexBuffer";
            bufferDesc.canHaveTypedViews = true;
            bufferDesc.canHaveRawViews = true;
            bufferDesc.canHaveUAVs = true;
            bufferDesc.isAccelStructBuildInput = gpu.rayTracingSupported;
            bufferDesc.keepInitialState = true;
            bufferDesc.initialState = caustica::rhi::ResourceStates::VertexBuffer;

            skinnedGpuMesh.vertexBuffer = gpu.device->createBuffer(bufferDesc);

            if (descriptorTable)
            {
                skinnedGpuMesh.vertexBufferDescriptor = std::make_shared<DescriptorHandle>(
                    descriptorTable->createDescriptorHandle(caustica::rhi::BindingSetItem::RawBuffer_SRV(0, skinnedGpuMesh.vertexBuffer)));
            }
        }
        if (descriptorTable
            && skinnedGpuMesh.vertexBuffer
            && !skinnedGpuMesh.vertexBufferDescriptor)
        {
            skinnedGpuMesh.vertexBufferDescriptor = std::make_shared<DescriptorHandle>(
                descriptorTable->createDescriptorHandle(
                    caustica::rhi::BindingSetItem::RawBuffer_SRV(
                        0,
                        skinnedGpuMesh.vertexBuffer)));
        }

        if (!skinnedGpu.jointBuffer)
        {
            caustica::rhi::BufferDesc jointBufferDesc;
            jointBufferDesc.debugName = "JointBuffer";
            jointBufferDesc.initialState = caustica::rhi::ResourceStates::ShaderResource;
            jointBufferDesc.keepInitialState = true;
            jointBufferDesc.canHaveRawViews = true;
            jointBufferDesc.byteSize = sizeof(dm::float4x4) * std::max<size_t>(1, proxy.jointMatrices.size());
            skinnedGpu.jointBuffer = gpu.device->createBuffer(jointBufferDesc);
        }

        if (!skinnedGpu.skinningBindingSet)
        {
            caustica::rhi::BindingSetDesc setDesc;
            setDesc.bindings = {
                caustica::rhi::BindingSetItem::PushConstants(0, sizeof(SkinningConstants)),
                caustica::rhi::BindingSetItem::RawBuffer_SRV(0, prototypeGpu.vertexBuffer),
                caustica::rhi::BindingSetItem::RawBuffer_SRV(1, skinnedGpu.jointBuffer),
                caustica::rhi::BindingSetItem::RawBuffer_UAV(0, skinnedGpuMesh.vertexBuffer)
            };

            skinnedGpu.skinningBindingSet = gpu.device->createBindingSet(setDesc, gpu.skinningBindingLayout);
        }
    }
}

void DispatchSkinnedMeshUpdates(
    SceneGpuResources& gpu,
    const scene::SceneRenderData& renderData,
    caustica::rhi::ICommandList* commandList,
    uint32_t /*frameIndex*/)
{
    bool skinningMarkerPlaced = false;
    std::vector<caustica::rhi::BufferHandle> skinnedVertexBuffersWritten;
    std::unordered_set<scene::MeshRenderResourceId, scene::MeshRenderResourceId::Hash> skinnedMeshesWritten;
    uint32_t skippedDuplicateSkinnedDispatchCount = 0;

    for (const scene::SkinnedMeshRenderProxy& proxy : renderData.skinnedMeshes)
    {
        const scene::MeshRenderResourceSnapshot* prototypeMesh = renderData.findMesh(proxy.prototypeMeshId);
        if (!proxy.needsSkinningUpdate || !renderData.findMesh(proxy.meshId) || !prototypeMesh
            || !proxy.meshId || !proxy.prototypeMeshId)
            continue;

        auto gpuIt = gpu.skinnedGpuByEntity.find(static_cast<uint32_t>(proxy.entity));
        if (gpuIt == gpu.skinnedGpuByEntity.end())
            continue;
        SkinnedMeshGpuState& skinnedGpu = gpuIt->second;
        if (!skinnedGpu.jointBuffer || !skinnedGpu.skinningBindingSet)
            continue;

        if (!skinnedMeshesWritten.insert(proxy.meshId).second)
        {
            skippedDuplicateSkinnedDispatchCount++;
            continue;
        }

        if (!skinningMarkerPlaced)
        {
            commandList->beginMarker("Skinning");
            skinningMarkerPlaced = true;
        }

        if (!proxy.debugName.empty())
            commandList->beginMarker(proxy.debugName.c_str());

        commandList->writeBuffer(
            skinnedGpu.jointBuffer,
            proxy.jointMatrices.data(),
            proxy.jointMatrices.size() * sizeof(float4x4));

        caustica::rhi::ComputeState state;
        state.pipeline = gpu.skinningPipeline;
        state.bindings = { skinnedGpu.skinningBindingSet };
        commandList->setComputeState(state);

        uint32_t vertexOffset = prototypeMesh->vertexOffset;
        const auto prototypeGpuIt = gpu.meshRegistry.find(proxy.prototypeMeshId);
        const auto skinnedGpuIt = gpu.meshRegistry.find(proxy.meshId);
        if (prototypeGpuIt == gpu.meshRegistry.end() || skinnedGpuIt == gpu.meshRegistry.end())
            continue;
        const MeshGpuRecord& prototypeBuffers = prototypeGpuIt->second;
        const MeshGpuRecord& skinnedBuffers = skinnedGpuIt->second;

        SkinningConstants constants{};
        constants.numVertices = prototypeMesh->totalVertices;

        constants.flags = 0;
        if (prototypeBuffers.hasAttribute(VertexAttribute::Normal)) constants.flags |= SkinningFlag_Normals;
        if (prototypeBuffers.hasAttribute(VertexAttribute::Tangent)) constants.flags |= SkinningFlag_Tangents;
        if (prototypeBuffers.hasAttribute(VertexAttribute::TexCoord1)) constants.flags |= SkinningFlag_TexCoord1;
        if (prototypeBuffers.hasAttribute(VertexAttribute::TexCoord2)) constants.flags |= SkinningFlag_TexCoord2;
        if (!skinnedGpu.skinningInitialized) constants.flags |= SkinningFlag_FirstFrame;
        skinnedGpu.skinningInitialized = true;

        constants.inputPositionOffset = uint32_t(prototypeBuffers.vertexBufferRange(VertexAttribute::Position).byteOffset + vertexOffset * sizeof(float3));
        constants.inputNormalOffset = uint32_t(prototypeBuffers.vertexBufferRange(VertexAttribute::Normal).byteOffset + vertexOffset * sizeof(uint32_t));
        constants.inputTangentOffset = uint32_t(prototypeBuffers.vertexBufferRange(VertexAttribute::Tangent).byteOffset + vertexOffset * sizeof(uint32_t));
        constants.inputTexCoord1Offset = uint32_t(prototypeBuffers.vertexBufferRange(VertexAttribute::TexCoord1).byteOffset + vertexOffset * sizeof(float2));
        constants.inputTexCoord2Offset = uint32_t(prototypeBuffers.vertexBufferRange(VertexAttribute::TexCoord2).byteOffset + vertexOffset * sizeof(float2));
        constants.inputJointIndexOffset = uint32_t(prototypeBuffers.vertexBufferRange(VertexAttribute::JointIndices).byteOffset + vertexOffset * sizeof(uint2));
        constants.inputJointWeightOffset = uint32_t(prototypeBuffers.vertexBufferRange(VertexAttribute::JointWeights).byteOffset + vertexOffset * sizeof(float4));
        constants.outputPositionOffset = uint32_t(skinnedBuffers.vertexBufferRange(VertexAttribute::Position).byteOffset);
        constants.outputPrevPositionOffset = uint32_t(skinnedBuffers.vertexBufferRange(VertexAttribute::PrevPosition).byteOffset);
        constants.outputNormalOffset = uint32_t(skinnedBuffers.vertexBufferRange(VertexAttribute::Normal).byteOffset);
        constants.outputTangentOffset = uint32_t(skinnedBuffers.vertexBufferRange(VertexAttribute::Tangent).byteOffset);
        constants.outputTexCoord1Offset = uint32_t(skinnedBuffers.vertexBufferRange(VertexAttribute::TexCoord1).byteOffset);
        constants.outputTexCoord2Offset = uint32_t(skinnedBuffers.vertexBufferRange(VertexAttribute::TexCoord2).byteOffset);
        commandList->setPushConstants(&constants, sizeof(constants));

        commandList->dispatch(dm::div_ceil(constants.numVertices, 256));
        skinnedVertexBuffersWritten.push_back(skinnedBuffers.vertexBuffer);

        if (!proxy.debugName.empty())
            commandList->endMarker();
    }

    if (!skinnedVertexBuffersWritten.empty())
    {
        for (const caustica::rhi::BufferHandle& vertexBuffer : skinnedVertexBuffersWritten)
            commandList->setBufferState(vertexBuffer, caustica::rhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();
    }

    if (skinningMarkerPlaced)
        commandList->endMarker();

    if (skippedDuplicateSkinnedDispatchCount > 0)
    {
        static bool duplicateSkinnedDispatchWarningShown = false;
        if (!duplicateSkinnedDispatchWarningShown)
        {
            caustica::warning("Skipped %u duplicate skinned mesh dispatches that shared an output mesh.",
                skippedDuplicateSkinnedDispatchCount);
            duplicateSkinnedDispatchWarningShown = true;
        }
    }
}

void ApplyMeshGpuUploadCommands(
    SceneGpuResources& gpu,
    std::span<const MeshGpuUploadCommand> commands,
    caustica::rhi::ICommandList* commandList)
{
    for (const MeshGpuUploadCommand& command : commands)
    {
        const auto meshGpuIt = gpu.meshRegistry.find(command.meshId);
        if (meshGpuIt == gpu.meshRegistry.end() || !meshGpuIt->second.vertexBuffer)
            continue;
        MeshGpuRecord& meshGpu = meshGpuIt->second;

        const uint64_t vertexOffset = command.vertexOffset;
        if (!command.positions.empty() && meshGpu.hasAttribute(VertexAttribute::Position))
        {
            commandList->writeBuffer(
                meshGpu.vertexBuffer,
                command.positions.data(),
                command.positions.size() * sizeof(dm::float3),
                meshGpu.vertexBufferRange(VertexAttribute::Position).byteOffset
                    + vertexOffset * sizeof(dm::float3));
        }
        if (!command.previousPositions.empty() && meshGpu.hasAttribute(VertexAttribute::PrevPosition))
        {
            commandList->writeBuffer(
                meshGpu.vertexBuffer,
                command.previousPositions.data(),
                command.previousPositions.size() * sizeof(dm::float3),
                meshGpu.vertexBufferRange(VertexAttribute::PrevPosition).byteOffset
                    + vertexOffset * sizeof(dm::float3));
        }
        if (!command.normals.empty() && meshGpu.hasAttribute(VertexAttribute::Normal))
        {
            commandList->writeBuffer(
                meshGpu.vertexBuffer,
                command.normals.data(),
                command.normals.size() * sizeof(uint32_t),
                meshGpu.vertexBufferRange(VertexAttribute::Normal).byteOffset
                    + vertexOffset * sizeof(uint32_t));
        }

        caustica::rhi::ResourceStates readyState =
            caustica::rhi::ResourceStates::VertexBuffer | caustica::rhi::ResourceStates::ShaderResource;
        if (meshGpu.vertexBuffer->getDesc().isAccelStructBuildInput)
            readyState = readyState | caustica::rhi::ResourceStates::AccelStructBuildInput;
        commandList->setBufferState(meshGpu.vertexBuffer, readyState);
    }
}

void PruneRemovedGpuResources(
    SceneGpuResources& gpu,
    const scene::SceneRenderData& renderData)
{
    std::unordered_set<scene::MeshRenderResourceId, scene::MeshRenderResourceId::Hash>
        liveMeshIds;
    liveMeshIds.reserve(renderData.meshSnapshots.size());
    for (const scene::MeshRenderResourceSnapshot& mesh : renderData.meshSnapshots)
    {
        if (mesh.id)
            liveMeshIds.insert(mesh.id);
    }
    std::erase_if(gpu.meshRegistry, [&liveMeshIds](const auto& entry) {
        return !liveMeshIds.contains(entry.first);
    });

    std::unordered_set<scene::MaterialRenderResourceId, scene::MaterialRenderResourceId::Hash>
        liveMaterialIds;
    liveMaterialIds.reserve(renderData.materialSnapshots.size());
    for (const scene::MaterialRenderResourceSnapshot& material : renderData.materialSnapshots)
    {
        if (material.id)
            liveMaterialIds.insert(material.id);
    }
    std::erase_if(gpu.materialRegistry, [&liveMaterialIds](const auto& entry) {
        return !liveMaterialIds.contains(entry.first);
    });
}

void UpdateGpuSceneBuffers(
    SceneGpuResources& gpu,
    const scene::SceneRenderData& renderData,
    IDescriptorTableManager* descriptorTable,
    caustica::rhi::ICommandList* commandList,
    uint32_t frameIndex,
    bool structureChanged,
    bool transformsChanged)
{
    gpu.enableBindlessResources = descriptorTable != nullptr;
    std::vector<MeshGpuUploadCommand> meshUploads = gpu.takePendingMeshUploads();
    for (const MeshGpuUploadCommand& upload : meshUploads)
    {
        if (!upload.recreateVertexBuffer)
            continue;
        const auto meshGpuIt = gpu.meshRegistry.find(upload.meshId);
        if (meshGpuIt == gpu.meshRegistry.end())
            continue;
        meshGpuIt->second.vertexBuffer = nullptr;
        meshGpuIt->second.vertexBufferDescriptor.reset();
        meshGpuIt->second.vertexBufferRanges.fill(caustica::rhi::BufferRange{});
    }
    bool materialsChanged = false;

    if (structureChanged || !meshUploads.empty())
    {
        if (structureChanged)
        {
            gpu.skinnedGpuByEntity.clear();
            PruneRemovedGpuResources(gpu, renderData);
        }
        EnsureMeshGpuBuffers(gpu, renderData, descriptorTable, commandList);
    }
    ApplyMeshGpuUploadCommands(gpu, meshUploads, commandList);

    const size_t allocationGranularity = 1024;
    bool arraysAllocated = false;

    if (gpu.enableBindlessResources && renderData.geometryCount > gpu.geometryData.size())
    {
        gpu.geometryData.resize(caustica::rhi::align<size_t>(renderData.geometryCount, allocationGranularity));
        gpu.geometryBuffer = CreateGeometryBuffer(gpu);
        arraysAllocated = true;
    }

    if (renderData.materialSnapshots.size() > gpu.materialData.size())
    {
        gpu.materialData.resize(caustica::rhi::align<size_t>(renderData.materialSnapshots.size(), allocationGranularity));
        if (gpu.enableBindlessResources)
            gpu.materialBuffer = CreateMaterialBuffer(gpu);
        arraysAllocated = true;
    }

    if (renderData.meshInstanceEntities.size() > gpu.instanceData.size())
    {
        gpu.instanceData.resize(caustica::rhi::align<size_t>(renderData.meshInstanceEntities.size(), allocationGranularity));
        gpu.instanceBuffer = CreateInstanceBuffer(gpu);
        arraysAllocated = true;
    }

    for (const scene::MaterialRenderResourceSnapshot& material : renderData.materialSnapshots)
    {
        if (!material.id)
            continue;

        const MaterialConstants& selectedConstants = gpu.useResourceDescriptorHeapBindless
            ? material.bindlessConstants
            : material.constants;
        const uint64_t contentHash = HashMaterialConstants(selectedConstants);
        MaterialGpuRecord& materialGpu = gpu.materialRegistry[material.id];
        const bool needsUpload =
            materialGpu.uploadedContentHash != contentHash
            || structureChanged
            || arraysAllocated;

        if (needsUpload)
            UpdateMaterial(gpu, material);

        if (!materialGpu.constantsBuffer)
            materialGpu.constantsBuffer = CreateMaterialConstantBuffer(gpu, material.debugName);

        if (needsUpload)
        {
            if (material.materialIndex >= gpu.materialData.size())
                continue;

            commandList->writeBuffer(materialGpu.constantsBuffer,
                &gpu.materialData[material.materialIndex],
                sizeof(MaterialConstants));

            materialGpu.uploadedContentHash = contentHash;
            materialsChanged = true;
        }
    }

    if (!gpu.geometryData.empty())
    {
        uint32_t geometryResourceIndex = 0;
        for (const auto& mesh : renderData.meshSnapshots)
        {
            if (arraysAllocated)
                break;

            for (const auto& geometry : mesh.geometries)
            {
                if (geometryResourceIndex >= gpu.geometryData.size())
                {
                    caustica::error("SceneGpuUpdater: geometry index %u out of range (size=%zu)",
                        geometryResourceIndex, gpu.geometryData.size());
                    break;
                }

                if (geometry.numIndices != gpu.geometryData[geometryResourceIndex].numIndices)
                {
                    arraysAllocated = true;
                    break;
                }
                ++geometryResourceIndex;
            }
        }
    }

    if (structureChanged || arraysAllocated)
    {
        for (const auto& mesh : renderData.meshSnapshots)
        {
            if (!mesh.upload)
                continue;

            gpu.meshRegistry[mesh.id].instanceBuffer = gpu.instanceBuffer;

            if (gpu.enableBindlessResources)
                UpdateGeometry(gpu, mesh);
        }

        if (gpu.enableBindlessResources)
            WriteGeometryBuffer(commandList, gpu);
    }

    if (structureChanged || transformsChanged || arraysAllocated)
    {
        uint32_t compactedGeometryInstanceIndex = 0;
        for (const scene::MeshInstanceRenderProxy& proxy : renderData.meshInstances)
        {
            UpdateInstance(gpu, proxy, compactedGeometryInstanceIndex);
            compactedGeometryInstanceIndex += proxy.geometryCount;
        }

        WriteInstanceBuffer(commandList, gpu);
    }

    if (gpu.enableBindlessResources && (materialsChanged || structureChanged || arraysAllocated))
        WriteMaterialBuffer(commandList, gpu);

    DispatchSkinnedMeshUpdates(gpu, renderData, commandList, frameIndex);
}

} // namespace

void SceneGpuUpdater::initialize(
    SceneGpuResources& gpu,
    caustica::rhi::IDevice* device,
    ShaderFactory& shaderFactory)
{
    gpu.clearSceneResources();
    gpu.device = device;
    gpu.rayTracingSupported = device->queryFeatureSupport(caustica::rhi::Feature::RayTracingAccelStruct);
    gpu.skinningShader = shaderFactory.createAutoShader(
        "engine/skinning_cs",
        "main",
        CAUSTICA_MAKE_PLATFORM_SHADER(g_skinning_cs),
        nullptr,
        caustica::rhi::ShaderType::Compute);

    caustica::rhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = caustica::rhi::ShaderType::Compute;
    layoutDesc.bindings = {
        caustica::rhi::BindingLayoutItem::PushConstants(0, sizeof(SkinningConstants)),
        caustica::rhi::BindingLayoutItem::RawBuffer_SRV(0),
        caustica::rhi::BindingLayoutItem::RawBuffer_SRV(1),
        caustica::rhi::BindingLayoutItem::RawBuffer_UAV(0)
    };
    gpu.skinningBindingLayout = device->createBindingLayout(layoutDesc);

    caustica::rhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { gpu.skinningBindingLayout };
    pipelineDesc.CS = gpu.skinningShader;
    gpu.skinningPipeline = device->createComputePipeline(pipelineDesc);
}

void SceneGpuUpdater::refresh(
    Scene& scene,
    SceneGpuResources& gpu,
    IDescriptorTableManager* descriptorTable,
    caustica::rhi::ICommandList* commandList,
    uint32_t frameIndex)
{
    assertRenderThread();

    if (commandList == nullptr)
        return;

    const GpuReadFrameScope gpuReadScope(scene, frameIndex);

    if (!scene.wasRenderSnapshotExtractedOnLogicThread(frameIndex))
    {
        caustica::warning(
            "SceneGpuUpdater::refresh: missing logic-thread extract for frame %u (render will use last published snapshot)",
            frameIndex);
    }

    const bool structureChanged = scene.hasSceneStructureChanged(frameIndex);
    const bool transformsChanged = scene.hasSceneTransformsChanged(frameIndex);
    const scene::SceneRenderData& renderData = scene.getRenderData();

    UpdateGpuSceneBuffers(
        gpu,
        renderData,
        descriptorTable,
        commandList,
        frameIndex,
        structureChanged,
        transformsChanged);
    scene.syncRenderSnapshotGpuIndices(frameIndex);
    if (structureChanged)
        scene.acknowledgeGpuStructureConsumed(frameIndex);
}

void SceneGpuUpdater::refreshAfterLoad(
    Scene& scene,
    const scene::SceneRenderData& renderData,
    SceneGpuResources& gpu,
    IDescriptorTableManager* descriptorTable,
    uint32_t frameIndex)
{
    if (!gpu.device->waitForIdle())
        return;

    caustica::rhi::CommandListHandle commandList = gpu.device->createCommandList();
    commandList->open();
    {
        EnsureMeshGpuBuffers(gpu, renderData, descriptorTable, commandList);
        UpdateGpuSceneBuffers(
            gpu,
            renderData,
            descriptorTable,
            commandList,
            frameIndex,
            /*structureChanged=*/true,
            /*transformsChanged=*/true);
    }
    commandList->close();
    gpu.device->executeCommandList(commandList);
    gpu.device->waitForIdle();
}

} // namespace caustica::render

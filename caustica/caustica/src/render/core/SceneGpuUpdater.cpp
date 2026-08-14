#include <render/core/SceneGpuUpdater.h>

#include <render/SceneGpuResources.h>
#include <render/core/StreamingUploadBudget.h>
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
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
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
    // bytes - using the padded range.byteSize reads past the end of CPU vectors.
    range.byteSize = size;
    currentBufferSize += caustica::rhi::align(size, size_t(16));
}

template <typename T>
inline void WriteAttributeRange(caustica::rhi::CommandList* commandList, caustica::rhi::Buffer* buffer,
    const std::vector<T>& data, const caustica::rhi::BufferRange& range)
{
    if (data.empty() || range.byteSize == 0)
        return;
    assert(range.byteSize == data.size() * sizeof(T));
    commandList->writeBuffer(buffer, data.data(), range.byteSize, range.byteOffset);
}

size_t SaturatingAdd(size_t lhs, size_t rhs)
{
    return rhs > std::numeric_limits<size_t>::max() - lhs
        ? std::numeric_limits<size_t>::max()
        : lhs + rhs;
}

template <typename T>
void AddVectorBytes(size_t& bytes, const std::vector<T>& data, size_t copies = 1)
{
    if (data.size() > std::numeric_limits<size_t>::max() / sizeof(T))
    {
        bytes = std::numeric_limits<size_t>::max();
        return;
    }

    const size_t oneCopy = data.size() * sizeof(T);
    for (size_t i = 0; i < copies; ++i)
        bytes = SaturatingAdd(bytes, oneCopy);
}

size_t MeshUploadBytes(const scene::MeshUploadBlob& upload)
{
    size_t bytes = 0;
    AddVectorBytes(bytes, upload.indexData);
    AddVectorBytes(bytes, upload.positionData, 2); // current + previous position
    AddVectorBytes(bytes, upload.normalData);
    AddVectorBytes(bytes, upload.tangentData);
    AddVectorBytes(bytes, upload.texcoord1Data);
    AddVectorBytes(bytes, upload.texcoord2Data);
    AddVectorBytes(bytes, upload.weightData);
    AddVectorBytes(bytes, upload.jointData);
    AddVectorBytes(bytes, upload.radiusData);
    return bytes;
}

size_t MeshUploadAllocationCount(const scene::MeshUploadBlob& upload)
{
    size_t count = upload.indexData.empty() ? 0 : 1;
    count += upload.positionData.empty() ? 0 : 2; // current + previous position
    count += upload.normalData.empty() ? 0 : 1;
    count += upload.tangentData.empty() ? 0 : 1;
    count += upload.texcoord1Data.empty() ? 0 : 1;
    count += upload.texcoord2Data.empty() ? 0 : 1;
    count += upload.weightData.empty() ? 0 : 1;
    count += upload.jointData.empty() ? 0 : 1;
    count += upload.radiusData.empty() ? 0 : 1;
    return count;
}

bool IsMeshGpuRecordReady(
    const MeshGpuRecord& record,
    const std::shared_ptr<const scene::MeshUploadBlob>& upload)
{
    if (!upload)
        return true;

    const bool indexReady = upload->indexData.empty() || record.indexBuffer != nullptr;
    const bool needsVertex = !upload->positionData.empty()
        || !upload->normalData.empty()
        || !upload->tangentData.empty()
        || !upload->texcoord1Data.empty()
        || !upload->texcoord2Data.empty()
        || !upload->weightData.empty()
        || !upload->jointData.empty()
        || !upload->radiusData.empty();
    return indexReady && (!needsVertex || record.vertexBuffer != nullptr);
}

struct MeshUploadPlan
{
    size_t end = 0;
    size_t bytes = 0;
    size_t allocationCount = 0;
};

MeshUploadPlan PlanMeshUpload(
    const scene::SceneRenderData& renderData,
    const SceneGpuResources& gpu,
    size_t meshBegin,
    size_t targetUploadBytes)
{
    const size_t meshCount = renderData.staticData().meshSnapshots.size();
    MeshUploadPlan plan{ std::min(meshBegin, meshCount), 0, 0 };
    if (meshBegin >= meshCount)
        return plan;

    const size_t target = std::max<size_t>(targetUploadBytes, 1);
    std::unordered_set<const scene::MeshUploadBlob*> plannedUploads;
    std::unordered_set<const scene::MeshUploadBlob*> gpuReadyUploads;
    plannedUploads.reserve(meshCount - meshBegin);
    gpuReadyUploads.reserve(gpu.meshRegistry.size());
    for (const auto& [id, candidate] : gpu.meshRegistry)
    {
        (void)id;
        if (candidate.uploadSource
            && IsMeshGpuRecordReady(candidate, candidate.uploadSource))
        {
            gpuReadyUploads.insert(candidate.uploadSource.get());
        }
    }

    for (size_t i = meshBegin; i < meshCount; ++i)
    {
        const auto& upload = renderData.staticData().meshSnapshots[i].upload;
        size_t addedBytes = 0;
        if (upload && plannedUploads.insert(upload.get()).second
            && !gpuReadyUploads.contains(upload.get()))
        {
            addedBytes = MeshUploadBytes(*upload);
            plan.allocationCount = SaturatingAdd(
                plan.allocationCount, MeshUploadAllocationCount(*upload));
        }

        // A single immutable BufferGroup may be larger than the target. It still
        // has to make forward progress, while the global streaming budget makes
        // sure that oversized submit runs without other uploads in flight.
        if (plan.end > meshBegin && addedBytes > target - std::min(plan.bytes, target))
            break;

        plan.bytes = SaturatingAdd(plan.bytes, addedBytes);
        plan.end = i + 1;
    }

    // Event-query accounting needs a non-zero weight even when this batch only
    // attaches mesh records to a BufferGroup uploaded by an earlier record.
    plan.bytes = std::max(plan.bytes, size_t(64 * 1024));
    return plan;
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

void WriteMaterialBuffer(caustica::rhi::CommandList* commandList, const SceneGpuResources& gpu)
{
    if (!gpu.materialBuffer || gpu.materialData.empty())
        return;
    commandList->writeBuffer(gpu.materialBuffer, gpu.materialData.data(),
        gpu.materialData.size() * sizeof(MaterialConstants));
}

void WriteGeometryBuffer(caustica::rhi::CommandList* commandList, const SceneGpuResources& gpu)
{
    if (!gpu.geometryBuffer || gpu.geometryData.empty())
        return;
    commandList->writeBuffer(gpu.geometryBuffer, gpu.geometryData.data(),
        gpu.geometryData.size() * sizeof(GeometryData));
}

void WriteInstanceBuffer(caustica::rhi::CommandList* commandList, const SceneGpuResources& gpu)
{
    if (!gpu.instanceBuffer || gpu.instanceData.empty())
        return;
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

    // Must match TLAS instanceID / MaterialGpuCache dense prefix - not a possibly
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

bool EnsureMeshGpuBuffers(
    SceneGpuResources& gpu,
    const scene::SceneRenderData& renderData,
    IDescriptorTableManager* descriptorTable,
    caustica::rhi::CommandList* commandList,
    size_t meshBegin = 0,
    size_t meshEnd = size_t(-1))
{
    const size_t meshCount = renderData.staticData().meshSnapshots.size();
    if (meshEnd > meshCount)
        meshEnd = meshCount;
    if (meshBegin > meshEnd)
        meshBegin = meshEnd;

    // Index immutable BufferGroups once. The old per-mesh registry scan made
    // shared-buffer scenes O(meshes^2), which is catastrophic long before the
    // renderer reaches million-mesh scale.
    std::unordered_map<const scene::MeshUploadBlob*, scene::MeshRenderResourceId> uploadedSources;
    uploadedSources.reserve(gpu.meshRegistry.size() + (meshEnd - meshBegin));
    for (const auto& [candidateId, candidate] : gpu.meshRegistry)
    {
        if (candidate.uploadSource && IsMeshGpuRecordReady(candidate, candidate.uploadSource))
            uploadedSources.try_emplace(candidate.uploadSource.get(), candidateId);
    }

    for (size_t meshIndex = meshBegin; meshIndex < meshEnd; ++meshIndex)
    {
        const auto& mesh = renderData.staticData().meshSnapshots[meshIndex];
        const auto& buffers = mesh.upload;

        if (!buffers)
            continue;

        MeshGpuRecord& meshGpu = gpu.meshRegistry[mesh.id];

        if (meshGpu.uploadSource != buffers)
        {
            // A structure refresh may replace the immutable upload blob. Drop
            // only the shared base-buffer view; BLAS/OMM state remains per mesh
            // and is rebuilt by the structure path.
            meshGpu.uploadSource = buffers;
            meshGpu.indexBuffer = nullptr;
            meshGpu.vertexBuffer = nullptr;
            meshGpu.indexBufferDescriptor.reset();
            meshGpu.vertexBufferDescriptor.reset();
            meshGpu.vertexBufferRanges = {};
            meshGpu.morphTargetBufferRanges.clear();

            // Reuse a buffer set already uploaded for another mesh range from
            // the same authoring BufferGroup. This is critical for glTF scenes
            // where hundreds of MeshInfo records share one large buffer group.
            if (const auto sourceIt = uploadedSources.find(buffers.get());
                sourceIt != uploadedSources.end())
            {
                const auto candidateIt = gpu.meshRegistry.find(sourceIt->second);
                if (candidateIt != gpu.meshRegistry.end())
                {
                    const MeshGpuRecord& candidate = candidateIt->second;
                    meshGpu.indexBuffer = candidate.indexBuffer;
                    meshGpu.vertexBuffer = candidate.vertexBuffer;
                    meshGpu.indexBufferDescriptor = candidate.indexBufferDescriptor;
                    meshGpu.vertexBufferDescriptor = candidate.vertexBufferDescriptor;
                    meshGpu.vertexBufferRanges = candidate.vertexBufferRanges;
                    meshGpu.morphTargetBufferRanges = candidate.morphTargetBufferRanges;
                }
            }
        }

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
                return false;
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
                return false;
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

        if (IsMeshGpuRecordReady(meshGpu, buffers))
            uploadedSources.try_emplace(buffers.get(), mesh.id);
    }

    // Skinned setup needs the full prototype mesh set; skip on partial upload batches.
    if (meshBegin != 0 || meshEnd != meshCount)
        return true;

    auto& skinnedGpuMap = gpu.skinnedGpuByEntity;
    for (const scene::SkinnedMeshRenderProxy& proxy : renderData.skinnedMeshes)
    {
        const std::span<const dm::float4x4> jointMatrices = renderData.jointMatrices(proxy);
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
            if (!skinnedGpuMesh.vertexBuffer)
            {
                caustica::error("Failed to create skinned vertex buffer (%zu bytes).", skinnedVertexBufferSize);
                return false;
            }

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
            jointBufferDesc.byteSize = sizeof(dm::float4x4) * std::max<size_t>(1, jointMatrices.size());
            skinnedGpu.jointBuffer = gpu.device->createBuffer(jointBufferDesc);
            if (!skinnedGpu.jointBuffer)
            {
                caustica::error("Failed to create skinning joint buffer (%llu bytes).",
                    static_cast<unsigned long long>(jointBufferDesc.byteSize));
                return false;
            }
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
            if (!skinnedGpu.skinningBindingSet)
            {
                caustica::error("Failed to create skinning binding set.");
                return false;
            }
        }
    }
    return true;
}

void DispatchSkinnedMeshUpdates(
    SceneGpuResources& gpu,
    const scene::SceneRenderData& renderData,
    caustica::rhi::CommandList* commandList,
    uint32_t /*frameIndex*/)
{
    // Skinning is optional for scenes that contain only static geometry. Never
    // submit a null PSO when an engine shader is missing or rejected by the
    // driver; doing so used to crash during startup before an error could be
    // reported. Skinned meshes remain in their uploaded/rest pose in this case.
    if (!gpu.skinningPipeline || !gpu.skinningBindingLayout)
        return;

    bool skinningMarkerPlaced = false;
    std::vector<caustica::rhi::BufferHandle> skinnedVertexBuffersWritten;
    std::unordered_set<scene::MeshRenderResourceId, scene::MeshRenderResourceId::Hash> skinnedMeshesWritten;
    uint32_t skippedDuplicateSkinnedDispatchCount = 0;

    for (const scene::SkinnedMeshRenderProxy& proxy : renderData.skinnedMeshes)
    {
        const std::span<const dm::float4x4> jointMatrices = renderData.jointMatrices(proxy);
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
            jointMatrices.data(),
            jointMatrices.size_bytes());

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
        if (!skinnedGpu.skinningInitialized || proxy.resetMotionHistory)
            constants.flags |= SkinningFlag_FirstFrame;
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
    caustica::rhi::CommandList* commandList)
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
    liveMeshIds.reserve(renderData.staticData().meshSnapshots.size());
    for (const scene::MeshRenderResourceSnapshot& mesh : renderData.staticData().meshSnapshots)
    {
        if (mesh.id)
            liveMeshIds.insert(mesh.id);
    }
    std::erase_if(gpu.meshRegistry, [&liveMeshIds](const auto& entry) {
        return !liveMeshIds.contains(entry.first);
    });

    std::unordered_set<scene::MaterialRenderResourceId, scene::MaterialRenderResourceId::Hash>
        liveMaterialIds;
    liveMaterialIds.reserve(renderData.staticData().materialSnapshots.size());
    for (const scene::MaterialRenderResourceSnapshot& material : renderData.staticData().materialSnapshots)
    {
        if (material.id)
            liveMaterialIds.insert(material.id);
    }
    std::erase_if(gpu.materialRegistry, [&liveMaterialIds](const auto& entry) {
        return !liveMaterialIds.contains(entry.first);
    });
}

bool UpdateGpuSceneBuffers(
    SceneGpuResources& gpu,
    const scene::SceneRenderData& renderData,
    IDescriptorTableManager* descriptorTable,
    caustica::rhi::CommandList* commandList,
    uint32_t frameIndex,
    bool structureChanged,
    bool transformsChanged,
    bool pruneRemovedResources = true)
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
            if (pruneRemovedResources)
                PruneRemovedGpuResources(gpu, renderData);
        }
        if (!EnsureMeshGpuBuffers(gpu, renderData, descriptorTable, commandList))
            return false;
    }
    ApplyMeshGpuUploadCommands(gpu, meshUploads, commandList);

    const size_t allocationGranularity = 1024;
    bool arraysAllocated = false;

    if (gpu.enableBindlessResources && renderData.staticData().geometryCount > gpu.geometryData.size())
    {
        gpu.geometryData.resize(caustica::rhi::align<size_t>(renderData.staticData().geometryCount, allocationGranularity));
        gpu.geometryBuffer = CreateGeometryBuffer(gpu);
        if (!gpu.geometryBuffer)
            return false;
        arraysAllocated = true;
    }

    if (renderData.staticData().materialSnapshots.size() > gpu.materialData.size())
    {
        gpu.materialData.resize(caustica::rhi::align<size_t>(renderData.staticData().materialSnapshots.size(), allocationGranularity));
        if (gpu.enableBindlessResources)
            gpu.materialBuffer = CreateMaterialBuffer(gpu);
        if (gpu.enableBindlessResources && !gpu.materialBuffer)
            return false;
        arraysAllocated = true;
    }

    if (renderData.meshInstanceEntities.size() > gpu.instanceData.size())
    {
        gpu.instanceData.resize(caustica::rhi::align<size_t>(renderData.meshInstanceEntities.size(), allocationGranularity));
        gpu.instanceBuffer = CreateInstanceBuffer(gpu);
        if (!gpu.instanceBuffer)
            return false;
        arraysAllocated = true;
    }

    for (const scene::MaterialRenderResourceSnapshot& material : renderData.staticData().materialSnapshots)
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

        if (!materialGpu.constantsBuffer)
            return false;

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
        for (const auto& mesh : renderData.staticData().meshSnapshots)
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
        for (const auto& mesh : renderData.staticData().meshSnapshots)
        {
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
    return gpu.device && gpu.device->isDeviceHealthy();
}

} // namespace

void SceneGpuUpdater::initialize(
    SceneGpuResources& gpu,
    caustica::rhi::Device* device,
    ShaderFactory& shaderFactory)
{
    gpu.clearSceneResources();
    gpu.device = device;
    if (device == nullptr)
    {
        caustica::error("SceneGpuUpdater: cannot initialize without a GPU device.");
        return;
    }

    gpu.rayTracingSupported = device->queryFeatureSupport(caustica::rhi::Feature::RayTracingAccelStruct);
    gpu.skinningShader = shaderFactory.createAutoShader(
        "engine/skinning_cs",
        "main",
        CAUSTICA_MAKE_PLATFORM_SHADER(g_skinning_cs),
        nullptr,
        caustica::rhi::ShaderType::Compute);
    if (!gpu.skinningShader)
    {
        caustica::error(
            "SceneGpuUpdater: engine/skinning_cs is unavailable; disabling GPU skinning instead of creating a null PSO.");
        return;
    }

    caustica::rhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = caustica::rhi::ShaderType::Compute;
    layoutDesc.bindings = {
        caustica::rhi::BindingLayoutItem::PushConstants(0, sizeof(SkinningConstants)),
        caustica::rhi::BindingLayoutItem::RawBuffer_SRV(0),
        caustica::rhi::BindingLayoutItem::RawBuffer_SRV(1),
        caustica::rhi::BindingLayoutItem::RawBuffer_UAV(0)
    };
    gpu.skinningBindingLayout = device->createBindingLayout(layoutDesc);
    if (!gpu.skinningBindingLayout)
    {
        caustica::error(
            "SceneGpuUpdater: failed to create the skinning binding layout; disabling GPU skinning.");
        gpu.skinningShader = nullptr;
        return;
    }

    caustica::rhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { gpu.skinningBindingLayout };
    pipelineDesc.CS = gpu.skinningShader;
    gpu.skinningPipeline = device->createComputePipeline(pipelineDesc);
    if (!gpu.skinningPipeline)
    {
        caustica::error(
            "SceneGpuUpdater: failed to create the skinning compute pipeline; disabling GPU skinning.");
        gpu.skinningBindingLayout = nullptr;
        gpu.skinningShader = nullptr;
    }
}

void SceneGpuUpdater::refresh(
    Scene& scene,
    SceneGpuResources& gpu,
    IDescriptorTableManager* descriptorTable,
    caustica::rhi::CommandList* commandList,
    uint32_t frameIndex)
{
    assertRenderThread();

    if (commandList == nullptr)
        return;

    const GpuReadFrameScope gpuReadScope(scene, frameIndex);

    // Async structure build owns mesh upload / AS / acknowledge via refreshAfterLoad.
    // Skip per-frame GPU scene updates so we don't race prune/upload against the old TLAS.
    if (scene.structureGpuBuildInFlight())
        return;

    if (!scene.wasRenderSnapshotExtractedOnLogicThread(frameIndex))
    {
        caustica::warning(
            "SceneGpuUpdater::refresh: missing logic-thread extract for frame %u (render will use last published snapshot)",
            frameIndex);
    }

    const bool structureChanged = scene.hasSceneStructureChanged(frameIndex);
    const bool transformsChanged = scene.hasSceneTransformsChanged(frameIndex);
    const scene::SceneRenderData& renderData = scene.getRenderData();

    (void)UpdateGpuSceneBuffers(
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

size_t SceneGpuUpdater::uploadMeshesAfterLoad(
    const scene::SceneRenderData& renderData,
    SceneGpuResources& gpu,
    IDescriptorTableManager* descriptorTable,
    size_t meshBegin,
    size_t targetUploadBytes)
{
    const size_t meshCount = renderData.staticData().meshSnapshots.size();
    if (meshBegin >= meshCount)
        return meshCount;

    const MeshUploadPlan plan = PlanMeshUpload(
        renderData, gpu, meshBegin, targetUploadBytes);
    if (plan.end <= meshBegin)
        return meshBegin;

    auto& budget = streamingUploadBudget();
    // Gate CreateCommittedResource + copy backlog (ADR 0001 R1); no waitForIdle.
    if (!budget.waitForBudget(gpu.device, plan.bytes))
        return meshBegin;

    caustica::rhi::CommandListParameters uploadParams;
    constexpr size_t kUploadChunkSize = 4 * 1024 * 1024;
    uploadParams.uploadChunkSize = kUploadChunkSize;
    // The byte planner and process-wide StreamingUploadBudget are the memory
    // authority. Keep the per-command-list pool large enough for one planned
    // batch (including chunk rounding), otherwise UploadManager tries to reuse
    // a chunk that belongs to the command list currently being recorded.
    const size_t kUploadHeadroom = plan.allocationCount
        > std::numeric_limits<size_t>::max() / kUploadChunkSize
        ? std::numeric_limits<size_t>::max()
        : plan.allocationCount * kUploadChunkSize;
    uploadParams.uploadMaxMemory = std::max(
        size_t(128) * 1024 * 1024,
        SaturatingAdd(plan.bytes, kUploadHeadroom));

    caustica::rhi::CommandListHandle commandList = gpu.device->createCommandList(uploadParams);
    if (!commandList)
    {
        caustica::error("uploadMeshesAfterLoad: failed to create upload command list at mesh %zu", meshBegin);
        return meshBegin;
    }
    if (!commandList->open())
    {
        caustica::error("uploadMeshesAfterLoad: failed to open upload command list at mesh %zu", meshBegin);
        return meshBegin;
    }
    if (!EnsureMeshGpuBuffers(gpu, renderData, descriptorTable, commandList, meshBegin, plan.end))
    {
        commandList->close();
        return meshBegin;
    }
    commandList->close();
    const uint64_t submission = gpu.device->executeCommandList(commandList);
    if ((gpu.device->getGraphicsAPI() == caustica::rhi::GraphicsAPI::D3D12 && submission == 0)
        || !gpu.device->isDeviceHealthy())
        return meshBegin;
    commandList = nullptr;

    budget.trackSubmit(gpu.device, plan.bytes);
    return plan.end;
}

bool SceneGpuUpdater::finalizeAfterLoad(
    Scene& scene,
    const scene::SceneRenderData& renderData,
    SceneGpuResources& gpu,
    IDescriptorTableManager* descriptorTable,
    uint32_t frameIndex,
    bool pruneRemovedResources)
{
    (void)scene;

    auto& budget = streamingUploadBudget();
    // Mesh uploads must be GPU-complete before scene buffers / AccelOnly AS build.
    if (!budget.waitAll(gpu.device))
        return false;

    const size_t finalizeBytes = std::max(
        size_t(1) * 1024 * 1024,
        (renderData.staticData().meshSnapshots.size() + renderData.staticData().materialSnapshots.size() + 1) * size_t(256));
    if (!budget.waitForBudget(gpu.device, finalizeBytes))
        return false;

    caustica::rhi::CommandListHandle commandList = gpu.device->createCommandList();
    if (!commandList)
    {
        caustica::error("finalizeAfterLoad: failed to create command list");
        return false;
    }
    if (!commandList->open())
    {
        caustica::error("finalizeAfterLoad: failed to open command list");
        return false;
    }
    if (!UpdateGpuSceneBuffers(
        gpu,
        renderData,
        descriptorTable,
        commandList,
        frameIndex,
        /*structureChanged=*/true,
        /*transformsChanged=*/true,
        pruneRemovedResources))
    {
        commandList->close();
        return false;
    }
    commandList->close();
    const uint64_t submission = gpu.device->executeCommandList(commandList);
    if ((gpu.device->getGraphicsAPI() == caustica::rhi::GraphicsAPI::D3D12 && submission == 0)
        || !gpu.device->isDeviceHealthy())
        return false;

    budget.trackSubmit(gpu.device, finalizeBytes);
    return budget.waitAll(gpu.device) && gpu.device->isDeviceHealthy();
}

bool SceneGpuUpdater::refreshAfterLoad(
    Scene& scene,
    const scene::SceneRenderData& renderData,
    SceneGpuResources& gpu,
    IDescriptorTableManager* descriptorTable,
    uint32_t frameIndex,
    bool pruneRemovedResources)
{
    constexpr size_t kMeshUploadTargetBytes = std::numeric_limits<size_t>::max();
    const size_t meshCount = renderData.staticData().meshSnapshots.size();
    for (size_t begin = 0; begin < meshCount; )
    {
        const size_t next = uploadMeshesAfterLoad(
            renderData, gpu, descriptorTable, begin, kMeshUploadTargetBytes);
        if (next <= begin)
            return false;
        begin = next;
    }

    return finalizeAfterLoad(scene, renderData, gpu, descriptorTable, frameIndex, pruneRemovedResources);
}

} // namespace caustica::render

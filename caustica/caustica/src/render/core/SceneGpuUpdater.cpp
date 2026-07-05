#include <render/core/SceneGpuUpdater.h>

#include <engine/RenderThread.h>
#include <render/SceneGpuResources.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <backend/IDescriptorTableManager.h>
#include <core/DescriptorHandle.h>
#include <core/log.h>
#include <rhi/common/misc.h>

#include <cassert>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace caustica::math;
#include <shaders/skinning_cb.h>

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

inline void AppendBufferRange(nvrhi::BufferRange& range, size_t size, uint64_t& currentBufferSize)
{
    range.byteOffset = currentBufferSize;
    range.byteSize = nvrhi::align(size, size_t(16));
    currentBufferSize += range.byteSize;
}

nvrhi::BufferHandle CreateMaterialBuffer(SceneGpuResources& gpu)
{
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(MaterialConstants) * gpu.materialData.size();
    bufferDesc.debugName = "BindlessMaterials";
    bufferDesc.structStride = sizeof(MaterialConstants);
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;

    return gpu.device->createBuffer(bufferDesc);
}

nvrhi::BufferHandle CreateGeometryBuffer(SceneGpuResources& gpu)
{
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(GeometryData) * gpu.geometryData.size();
    bufferDesc.debugName = "BindlessGeometry";
    bufferDesc.structStride = sizeof(GeometryData);
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;

    return gpu.device->createBuffer(bufferDesc);
}

nvrhi::BufferHandle CreateInstanceBuffer(SceneGpuResources& gpu)
{
    const bool needStructuredBuffer = gpu.device->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11;

    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(InstanceData) * gpu.instanceData.size();
    bufferDesc.debugName = "Instances";
    bufferDesc.structStride = needStructuredBuffer ? sizeof(InstanceData) : 0;
    bufferDesc.canHaveRawViews = true;
    bufferDesc.canHaveUAVs = true;
    bufferDesc.isVertexBuffer = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    bufferDesc.keepInitialState = true;

    return gpu.device->createBuffer(bufferDesc);
}

nvrhi::BufferHandle CreateMaterialConstantBuffer(SceneGpuResources& gpu, const std::string& debugName)
{
    nvrhi::BufferDesc bufferDesc;
    bufferDesc.byteSize = sizeof(MaterialConstants);
    bufferDesc.debugName = debugName;
    bufferDesc.isConstantBuffer = true;
    bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    bufferDesc.keepInitialState = true;

    return gpu.device->createBuffer(bufferDesc);
}

void WriteMaterialBuffer(nvrhi::ICommandList* commandList, const SceneGpuResources& gpu)
{
    commandList->writeBuffer(gpu.materialBuffer, gpu.materialData.data(),
        gpu.materialData.size() * sizeof(MaterialConstants));
}

void WriteGeometryBuffer(nvrhi::ICommandList* commandList, const SceneGpuResources& gpu)
{
    commandList->writeBuffer(gpu.geometryBuffer, gpu.geometryData.data(),
        gpu.geometryData.size() * sizeof(GeometryData));
}

void WriteInstanceBuffer(nvrhi::ICommandList* commandList, const SceneGpuResources& gpu)
{
    commandList->writeBuffer(gpu.instanceBuffer, gpu.instanceData.data(),
        gpu.instanceData.size() * sizeof(InstanceData));
}

void UpdateMaterial(SceneGpuResources& gpu, const std::shared_ptr<Material>& material)
{
    if (!material || material->materialID >= gpu.materialData.size())
        return;

    material->FillConstantBuffer(gpu.materialData[material->materialID], gpu.useResourceDescriptorHeapBindless);
}

void UpdateGeometry(SceneGpuResources& gpu, const std::shared_ptr<MeshInfo>& mesh)
{
    for (const auto& geometry : mesh->geometries)
    {
        const uint32_t indexOffset = mesh->indexOffset + geometry->indexOffsetInMesh;
        const uint32_t vertexOffset = mesh->vertexOffset + geometry->vertexOffsetInMesh;

        GeometryData& gdata = gpu.geometryData[geometry->globalGeometryIndex];
        gdata.numIndices = geometry->numIndices;
        gdata.numVertices = geometry->numVertices;
        gdata.indexBufferIndex = mesh->buffers->indexBufferDescriptor ? mesh->buffers->indexBufferDescriptor->Get() : -1;
        gdata.indexOffset = indexOffset * sizeof(uint32_t);
        gdata.vertexBufferIndex = mesh->buffers->vertexBufferDescriptor ? mesh->buffers->vertexBufferDescriptor->Get() : -1;
        gdata.positionOffset = mesh->buffers->hasAttribute(VertexAttribute::Position)
            ? uint32_t(vertexOffset * sizeof(float3) + mesh->buffers->getVertexBufferRange(VertexAttribute::Position).byteOffset) : ~0u;
        gdata.prevPositionOffset = mesh->buffers->hasAttribute(VertexAttribute::PrevPosition)
            ? uint32_t(vertexOffset * sizeof(float3) + mesh->buffers->getVertexBufferRange(VertexAttribute::PrevPosition).byteOffset) : ~0u;
        gdata.texCoord1Offset = mesh->buffers->hasAttribute(VertexAttribute::TexCoord1)
            ? uint32_t(vertexOffset * sizeof(float2) + mesh->buffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset) : ~0u;
        gdata.texCoord2Offset = mesh->buffers->hasAttribute(VertexAttribute::TexCoord2)
            ? uint32_t(vertexOffset * sizeof(float2) + mesh->buffers->getVertexBufferRange(VertexAttribute::TexCoord2).byteOffset) : ~0u;
        gdata.normalOffset = mesh->buffers->hasAttribute(VertexAttribute::Normal)
            ? uint32_t(vertexOffset * sizeof(uint32_t) + mesh->buffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset) : ~0u;
        gdata.tangentOffset = mesh->buffers->hasAttribute(VertexAttribute::Tangent)
            ? uint32_t(vertexOffset * sizeof(uint32_t) + mesh->buffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset) : ~0u;
        gdata.curveRadiusOffset = mesh->buffers->hasAttribute(VertexAttribute::CurveRadius)
            ? uint32_t(vertexOffset * sizeof(float) + mesh->buffers->getVertexBufferRange(VertexAttribute::CurveRadius).byteOffset) : ~0u;
        gdata.materialIndex = geometry->material ? geometry->material->materialID : ~0u;
    }
}

void UpdateInstance(Scene& scene, ecs::Entity entity)
{
    if (!ecs::isValid(entity))
        return;

    const auto* entityWorld = scene.GetEntityWorld();
    if (!entityWorld)
        return;

    const auto& world = entityWorld->world();
    const auto* meshComp = world.get<scene::MeshInstanceComponent>(entity);
    const auto* global = world.get<scene::GlobalTransformComponent>(entity);
    if (!meshComp || !meshComp->mesh || !global || meshComp->instanceIndex < 0)
        return;

    SceneGpuResources& gpu = scene.GetGpuResources();
    if (meshComp->instanceIndex < 0 || static_cast<size_t>(meshComp->instanceIndex) >= gpu.instanceData.size())
        return;

    InstanceData& idata = gpu.instanceData[meshComp->instanceIndex];
    affineToColumnMajor(global->transformFloat, idata.transform);
    affineToColumnMajor(global->previousTransformFloat, idata.prevTransform);

    const auto& mesh = meshComp->mesh;
    idata.firstGeometryInstanceIndex = meshComp->geometryInstanceIndex;
    idata.numGeometries = uint32_t(mesh->geometries.size());
    idata.firstGeometryIndex = idata.numGeometries > 0 ? mesh->geometries[0]->globalGeometryIndex : -1;
    idata.flags = 0u;

    if (mesh->type == MeshType::CurveDisjointOrthogonalTriangleStrips)
    {
        idata.flags |= InstanceFlags_CurveDisjointOrthogonalTriangleStrips;
    }
    else if (mesh->type == MeshType::CurveLinearSweptSpheres)
    {
        idata.flags |= InstanceFlags_CurveLinearSweptSpheres;
    }
}

void EnsureMeshGpuBuffers(Scene& scene, nvrhi::ICommandList* commandList)
{
    SceneGpuResources& gpu = scene.GetGpuResources();
    IDescriptorTableManager* descriptorTable = scene.getDescriptorTableManager();

    for (const auto& mesh : scene.GetMeshes())
    {
        auto buffers = mesh->buffers;

        if (!buffers)
            continue;

        if (!buffers->indexData.empty() && !buffers->indexBuffer)
        {
            nvrhi::BufferDesc bufferDesc;
            bufferDesc.isIndexBuffer = true;
            bufferDesc.byteSize = buffers->indexData.size() * sizeof(uint32_t);
            bufferDesc.debugName = "IndexBuffer";
            bufferDesc.canHaveTypedViews = true;
            bufferDesc.canHaveRawViews = true;
            bufferDesc.format = nvrhi::Format::R32_UINT;
            bufferDesc.isAccelStructBuildInput = gpu.rayTracingSupported;

            buffers->indexBuffer = gpu.device->createBuffer(bufferDesc);

            if (descriptorTable)
            {
                buffers->indexBufferDescriptor = std::make_shared<DescriptorHandle>(
                    descriptorTable->createDescriptorHandle(nvrhi::BindingSetItem::RawBuffer_SRV(0, buffers->indexBuffer)));
            }

            commandList->beginTrackingBufferState(buffers->indexBuffer, nvrhi::ResourceStates::Common);
            commandList->writeBuffer(buffers->indexBuffer, buffers->indexData.data(), buffers->indexData.size() * sizeof(uint32_t));

            nvrhi::ResourceStates state = nvrhi::ResourceStates::IndexBuffer | nvrhi::ResourceStates::ShaderResource;
            if (bufferDesc.isAccelStructBuildInput)
                state = state | nvrhi::ResourceStates::AccelStructBuildInput;

            commandList->setPermanentBufferState(buffers->indexBuffer, state);
            commandList->commitBarriers();
        }

        if (!buffers->vertexBuffer)
        {
            nvrhi::BufferDesc bufferDesc;
            bufferDesc.isVertexBuffer = true;
            bufferDesc.byteSize = 0;
            bufferDesc.debugName = "VertexBuffer";
            bufferDesc.canHaveTypedViews = true;
            bufferDesc.canHaveRawViews = true;
            bufferDesc.isAccelStructBuildInput = gpu.rayTracingSupported;

            nvrhi::ResourceStates state = nvrhi::ResourceStates::VertexBuffer | nvrhi::ResourceStates::ShaderResource;
            if (bufferDesc.isAccelStructBuildInput)
                state = state | nvrhi::ResourceStates::AccelStructBuildInput;
            bufferDesc.initialState = state;
            bufferDesc.keepInitialState = true;

            if (!buffers->positionData.empty())
            {
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::Position),
                    buffers->positionData.size() * sizeof(buffers->positionData[0]), bufferDesc.byteSize);
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::PrevPosition),
                    buffers->positionData.size() * sizeof(buffers->positionData[0]), bufferDesc.byteSize);
            }

            if (!buffers->normalData.empty())
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::Normal), buffers->normalData.size() * sizeof(buffers->normalData[0]), bufferDesc.byteSize);
            if (!buffers->tangentData.empty())
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::Tangent), buffers->tangentData.size() * sizeof(buffers->tangentData[0]), bufferDesc.byteSize);
            if (!buffers->texcoord1Data.empty())
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::TexCoord1), buffers->texcoord1Data.size() * sizeof(buffers->texcoord1Data[0]), bufferDesc.byteSize);
            if (!buffers->texcoord2Data.empty())
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::TexCoord2), buffers->texcoord2Data.size() * sizeof(buffers->texcoord2Data[0]), bufferDesc.byteSize);
            if (!buffers->weightData.empty())
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::JointWeights), buffers->weightData.size() * sizeof(buffers->weightData[0]), bufferDesc.byteSize);
            if (!buffers->jointData.empty())
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::JointIndices), buffers->jointData.size() * sizeof(buffers->jointData[0]), bufferDesc.byteSize);
            if (!buffers->radiusData.empty())
                AppendBufferRange(buffers->getVertexBufferRange(VertexAttribute::CurveRadius), buffers->radiusData.size() * sizeof(buffers->radiusData[0]), bufferDesc.byteSize);

            if (bufferDesc.byteSize == 0)
                continue;

            buffers->vertexBuffer = gpu.device->createBuffer(bufferDesc);
            if (descriptorTable)
            {
                buffers->vertexBufferDescriptor = std::make_shared<DescriptorHandle>(
                    descriptorTable->createDescriptorHandle(nvrhi::BindingSetItem::RawBuffer_SRV(0, buffers->vertexBuffer)));
            }

            commandList->beginTrackingBufferState(buffers->vertexBuffer, nvrhi::ResourceStates::Common);

            if (!buffers->positionData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::Position);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->positionData.data(), range.byteSize, range.byteOffset);

                const auto& prevRange = buffers->getVertexBufferRange(VertexAttribute::PrevPosition);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->positionData.data(), prevRange.byteSize, prevRange.byteOffset);
            }

            if (!buffers->normalData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::Normal);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->normalData.data(), range.byteSize, range.byteOffset);
            }
            if (!buffers->tangentData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::Tangent);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->tangentData.data(), range.byteSize, range.byteOffset);
            }
            if (!buffers->texcoord1Data.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::TexCoord1);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->texcoord1Data.data(), range.byteSize, range.byteOffset);
            }
            if (!buffers->texcoord2Data.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::TexCoord2);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->texcoord2Data.data(), range.byteSize, range.byteOffset);
            }
            if (!buffers->weightData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::JointWeights);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->weightData.data(), range.byteSize, range.byteOffset);
            }
            if (!buffers->jointData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::JointIndices);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->jointData.data(), range.byteSize, range.byteOffset);
            }
            if (!buffers->radiusData.empty())
            {
                const auto& range = buffers->getVertexBufferRange(VertexAttribute::CurveRadius);
                commandList->writeBuffer(buffers->vertexBuffer, buffers->radiusData.data(), range.byteSize, range.byteOffset);
            }

            commandList->setBufferState(buffers->vertexBuffer, state);
            commandList->commitBarriers();
        }
    }

    auto* entityWorld = scene.GetEntityWorld();
    for (const ecs::Entity entity : scene.GetSkinnedMeshInstances())
    {
        if (!entityWorld)
            continue;

        auto& world = entityWorld->world();
        auto* skinned = world.get<scene::SkinnedMeshComponent>(entity);
        auto* meshComp = world.get<scene::MeshInstanceComponent>(entity);
        auto* skinnedGpu = world.get<scene::SkinnedMeshGpuComponent>(entity);
        if (!skinned || !meshComp || !meshComp->mesh || !skinnedGpu || !skinned->prototypeMesh)
            continue;

        const auto& skinnedMesh = meshComp->mesh;

        if (!skinnedMesh->buffers)
        {
            skinnedMesh->buffers = std::make_shared<BufferGroup>();

            const uint32_t totalVertices = skinnedMesh->totalVertices;

            skinnedMesh->buffers->indexBuffer = skinned->prototypeMesh->buffers->indexBuffer;
            skinnedMesh->buffers->indexBufferDescriptor = skinned->prototypeMesh->buffers->indexBufferDescriptor;

            const auto& prototypeBuffers = skinned->prototypeMesh->buffers;
            const auto& skinnedBuffers = skinnedMesh->buffers;

            size_t skinnedVertexBufferSize = 0;
            assert(prototypeBuffers->hasAttribute(VertexAttribute::Position));

            AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::Position), totalVertices * sizeof(float3), skinnedVertexBufferSize);
            AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::PrevPosition), totalVertices * sizeof(float3), skinnedVertexBufferSize);

            if (prototypeBuffers->hasAttribute(VertexAttribute::Normal))
                AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::Normal), totalVertices * sizeof(uint32_t), skinnedVertexBufferSize);
            if (prototypeBuffers->hasAttribute(VertexAttribute::Tangent))
                AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::Tangent), totalVertices * sizeof(uint32_t), skinnedVertexBufferSize);
            if (prototypeBuffers->hasAttribute(VertexAttribute::TexCoord1))
                AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::TexCoord1), totalVertices * sizeof(float2), skinnedVertexBufferSize);
            if (prototypeBuffers->hasAttribute(VertexAttribute::TexCoord2))
                AppendBufferRange(skinnedBuffers->getVertexBufferRange(VertexAttribute::TexCoord2), totalVertices * sizeof(float2), skinnedVertexBufferSize);

            nvrhi::BufferDesc bufferDesc;
            bufferDesc.isVertexBuffer = true;
            bufferDesc.byteSize = skinnedVertexBufferSize;
            bufferDesc.debugName = "SkinnedVertexBuffer";
            bufferDesc.canHaveTypedViews = true;
            bufferDesc.canHaveRawViews = true;
            bufferDesc.canHaveUAVs = true;
            bufferDesc.isAccelStructBuildInput = gpu.rayTracingSupported;
            bufferDesc.keepInitialState = true;
            bufferDesc.initialState = nvrhi::ResourceStates::VertexBuffer;

            skinnedBuffers->vertexBuffer = gpu.device->createBuffer(bufferDesc);

            if (descriptorTable)
            {
                skinnedBuffers->vertexBufferDescriptor = std::make_shared<DescriptorHandle>(
                    descriptorTable->createDescriptorHandle(nvrhi::BindingSetItem::RawBuffer_SRV(0, skinnedBuffers->vertexBuffer)));
            }
        }

        if (!skinnedGpu->jointBuffer)
        {
            nvrhi::BufferDesc jointBufferDesc;
            jointBufferDesc.debugName = "JointBuffer";
            jointBufferDesc.initialState = nvrhi::ResourceStates::ShaderResource;
            jointBufferDesc.keepInitialState = true;
            jointBufferDesc.canHaveRawViews = true;
            jointBufferDesc.byteSize = sizeof(dm::float4x4) * skinned->joints.size();
            skinnedGpu->jointBuffer = gpu.device->createBuffer(jointBufferDesc);
        }

        if (!skinnedGpu->skinningBindingSet)
        {
            const auto& prototypeBuffers = skinned->prototypeMesh->buffers;
            const auto& skinnedBuffers = skinnedMesh->buffers;

            nvrhi::BindingSetDesc setDesc;
            setDesc.bindings = {
                nvrhi::BindingSetItem::PushConstants(0, sizeof(SkinningConstants)),
                nvrhi::BindingSetItem::RawBuffer_SRV(0, prototypeBuffers->vertexBuffer),
                nvrhi::BindingSetItem::RawBuffer_SRV(1, skinnedGpu->jointBuffer),
                nvrhi::BindingSetItem::RawBuffer_UAV(0, skinnedBuffers->vertexBuffer)
            };

            skinnedGpu->skinningBindingSet = gpu.device->createBindingSet(setDesc, gpu.skinningBindingLayout);
        }
    }
}

void DispatchSkinnedMeshUpdates(Scene& scene, nvrhi::ICommandList* commandList, uint32_t frameIndex)
{
    SceneGpuResources& gpu = scene.GetGpuResources();
    auto* entityWorld = scene.GetEntityWorld();
    if (!entityWorld)
        return;

    bool skinningMarkerPlaced = false;
    std::vector<dm::float4x4> jointMatrices;
    std::vector<nvrhi::BufferHandle> skinnedVertexBuffersWritten;
    std::unordered_set<const MeshInfo*> skinnedMeshesWritten;
    uint32_t skippedDuplicateSkinnedDispatchCount = 0;

    for (const ecs::Entity entity : scene.GetSkinnedMeshInstances())
    {
        auto& world = entityWorld->world();
        auto* skinned = world.get<scene::SkinnedMeshComponent>(entity);
        auto* meshComp = world.get<scene::MeshInstanceComponent>(entity);
        auto* skinnedGpu = world.get<scene::SkinnedMeshGpuComponent>(entity);
        if (!skinned || !meshComp || !meshComp->mesh || !skinnedGpu || !skinned->prototypeMesh)
            continue;

        if (skinned->lastUpdateFrameIndex != scene::kForceSkinnedMeshUpdateFrameIndex
            && skinned->lastUpdateFrameIndex + 1 < frameIndex)
            continue;

        if (!skinnedMeshesWritten.insert(meshComp->mesh.get()).second)
        {
            skippedDuplicateSkinnedDispatchCount++;
            continue;
        }

        const auto* ownerGlobal = world.get<scene::GlobalTransformComponent>(entity);
        if (!ownerGlobal)
            continue;

        if (!skinningMarkerPlaced)
        {
            commandList->beginMarker("Skinning");
            skinningMarkerPlaced = true;
        }

        const auto* name = world.get<scene::NameComponent>(entity);
        const std::string groupName = name ? name->value : std::string{};
        if (!groupName.empty())
            commandList->beginMarker(groupName.c_str());

        jointMatrices.resize(skinned->joints.size());

        dm::daffine3 worldToRoot = inverse(ownerGlobal->transform);

        for (size_t i = 0; i < skinned->joints.size(); i++)
        {
            const ecs::Entity jointEntity = skinned->joints[i].jointEntity;
            const auto* jointGlobal = world.get<scene::GlobalTransformComponent>(jointEntity);
            if (!jointGlobal)
                continue;

            dm::float4x4 jointMatrix = dm::affineToHomogeneous(dm::affine3(jointGlobal->transform * worldToRoot));
            jointMatrix = skinned->joints[i].inverseBindMatrix * jointMatrix;
            jointMatrices[i] = jointMatrix;
        }

        commandList->writeBuffer(skinnedGpu->jointBuffer, jointMatrices.data(), jointMatrices.size() * sizeof(float4x4));

        nvrhi::ComputeState state;
        state.pipeline = gpu.skinningPipeline;
        state.bindings = { skinnedGpu->skinningBindingSet };
        commandList->setComputeState(state);

        uint32_t vertexOffset = skinned->prototypeMesh->vertexOffset;
        const auto& prototypeBuffers = skinned->prototypeMesh->buffers;
        const auto& skinnedBuffers = meshComp->mesh->buffers;

        SkinningConstants constants{};
        constants.numVertices = skinned->prototypeMesh->totalVertices;

        constants.flags = 0;
        if (prototypeBuffers->hasAttribute(VertexAttribute::Normal)) constants.flags |= SkinningFlag_Normals;
        if (prototypeBuffers->hasAttribute(VertexAttribute::Tangent)) constants.flags |= SkinningFlag_Tangents;
        if (prototypeBuffers->hasAttribute(VertexAttribute::TexCoord1)) constants.flags |= SkinningFlag_TexCoord1;
        if (prototypeBuffers->hasAttribute(VertexAttribute::TexCoord2)) constants.flags |= SkinningFlag_TexCoord2;
        if (!skinnedGpu->skinningInitialized) constants.flags |= SkinningFlag_FirstFrame;
        skinnedGpu->skinningInitialized = true;

        constants.inputPositionOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::Position).byteOffset + vertexOffset * sizeof(float3));
        constants.inputNormalOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset + vertexOffset * sizeof(uint32_t));
        constants.inputTangentOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset + vertexOffset * sizeof(uint32_t));
        constants.inputTexCoord1Offset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset + vertexOffset * sizeof(float2));
        constants.inputTexCoord2Offset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::TexCoord2).byteOffset + vertexOffset * sizeof(float2));
        constants.inputJointIndexOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::JointIndices).byteOffset + vertexOffset * sizeof(uint2));
        constants.inputJointWeightOffset = uint32_t(prototypeBuffers->getVertexBufferRange(VertexAttribute::JointWeights).byteOffset + vertexOffset * sizeof(float4));
        constants.outputPositionOffset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::Position).byteOffset);
        constants.outputPrevPositionOffset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::PrevPosition).byteOffset);
        constants.outputNormalOffset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::Normal).byteOffset);
        constants.outputTangentOffset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::Tangent).byteOffset);
        constants.outputTexCoord1Offset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::TexCoord1).byteOffset);
        constants.outputTexCoord2Offset = uint32_t(skinnedBuffers->getVertexBufferRange(VertexAttribute::TexCoord2).byteOffset);
        commandList->setPushConstants(&constants, sizeof(constants));

        commandList->dispatch(dm::div_ceil(constants.numVertices, 256));
        skinned->lastUpdateFrameIndex = frameIndex;
        skinnedVertexBuffersWritten.push_back(skinnedBuffers->vertexBuffer);

        if (!groupName.empty())
            commandList->endMarker();
    }

    if (!skinnedVertexBuffersWritten.empty())
    {
        for (const nvrhi::BufferHandle& vertexBuffer : skinnedVertexBuffersWritten)
            commandList->setBufferState(vertexBuffer, nvrhi::ResourceStates::UnorderedAccess);
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

void UpdateGpuSceneBuffers(Scene& scene, nvrhi::ICommandList* commandList, uint32_t frameIndex, bool structureChanged, bool transformsChanged)
{
    SceneGpuResources& gpu = scene.GetGpuResources();
    bool materialsChanged = false;

    if (structureChanged)
        EnsureMeshGpuBuffers(scene, commandList);

    const size_t allocationGranularity = 1024;
    bool arraysAllocated = false;

    if (gpu.enableBindlessResources && scene.GetGeometryCount() > gpu.geometryData.size())
    {
        gpu.geometryData.resize(nvrhi::align<size_t>(scene.GetGeometryCount(), allocationGranularity));
        gpu.geometryBuffer = CreateGeometryBuffer(gpu);
        arraysAllocated = true;
    }

    if (scene.GetMaterials().size() > gpu.materialData.size())
    {
        gpu.materialData.resize(nvrhi::align<size_t>(scene.GetMaterials().size(), allocationGranularity));
        if (gpu.enableBindlessResources)
            gpu.materialBuffer = CreateMaterialBuffer(gpu);
        arraysAllocated = true;
    }

    if (scene.GetMeshInstances().size() > gpu.instanceData.size())
    {
        gpu.instanceData.resize(nvrhi::align<size_t>(scene.GetMeshInstances().size(), allocationGranularity));
        gpu.instanceBuffer = CreateInstanceBuffer(gpu);
        arraysAllocated = true;
    }

    for (const auto& material : scene.GetMaterials())
    {
        if (material->dirty || structureChanged || arraysAllocated)
            UpdateMaterial(gpu, material);

        if (!material->materialConstants)
        {
            material->materialConstants = CreateMaterialConstantBuffer(gpu, material->name);
            material->dirty = true;
        }

        if (material->dirty)
        {
            if (material->materialID >= gpu.materialData.size())
                continue;

            commandList->writeBuffer(material->materialConstants,
                &gpu.materialData[material->materialID],
                sizeof(MaterialConstants));

            material->dirty = false;
            materialsChanged = true;
        }
    }

    if (!gpu.geometryData.empty())
    {
        uint32_t geometryResourceIndex = 0;
        for (const auto& mesh : scene.GetMeshes())
        {
            if (arraysAllocated)
                break;

            if (!mesh)
                continue;

            for (const auto& geometry : mesh->geometries)
            {
                if (geometryResourceIndex >= gpu.geometryData.size())
                {
                    caustica::error("SceneGpuUpdater: geometry index %u out of range (size=%zu)",
                        geometryResourceIndex, gpu.geometryData.size());
                    break;
                }

                if (geometry->numIndices != gpu.geometryData[geometryResourceIndex].numIndices)
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
        for (const auto& mesh : scene.GetMeshes())
        {
            if (!mesh || !mesh->buffers)
                continue;

            mesh->buffers->instanceBuffer = gpu.instanceBuffer;

            if (gpu.enableBindlessResources)
                UpdateGeometry(gpu, mesh);
        }

        if (gpu.enableBindlessResources)
            WriteGeometryBuffer(commandList, gpu);
    }

    if (structureChanged || transformsChanged || arraysAllocated)
    {
        for (const ecs::Entity entity : scene.GetMeshInstances())
            UpdateInstance(scene, entity);

        WriteInstanceBuffer(commandList, gpu);
    }

    if (gpu.enableBindlessResources && (materialsChanged || structureChanged || arraysAllocated))
        WriteMaterialBuffer(commandList, gpu);

    DispatchSkinnedMeshUpdates(scene, commandList, frameIndex);
}

} // namespace

void SceneGpuUpdater::refresh(Scene& scene, nvrhi::ICommandList* commandList, uint32_t frameIndex)
{
    if (commandList == nullptr)
        return;

    auto* entityWorld = scene.GetEntityWorld();
    if (entityWorld == nullptr)
        return;

    const GpuReadFrameScope gpuReadScope(scene, frameIndex);

    if (IsRenderThread())
    {
        if (!scene.wasRenderSnapshotExtractedOnLogicThread(frameIndex))
            caustica::warning("SceneGpuUpdater::refresh: missing logic-thread extract for frame %u", frameIndex);
    }
    else if (!scene.wasRenderSnapshotExtractedOnLogicThread(frameIndex))
    {
        if (entityWorld->hasPendingStructureChanges() || entityWorld->hasPendingTransformChanges())
            scene.RefreshSceneWorld(frameIndex);

        scene.PublishRenderSnapshot(frameIndex);
    }

    const bool structureChanged = scene.HasSceneStructureChanged(frameIndex);
    const bool transformsChanged = scene.HasSceneTransformsChanged(frameIndex);

    UpdateGpuSceneBuffers(scene, commandList, frameIndex, structureChanged, transformsChanged);
    scene.syncRenderSnapshotGpuIndices(frameIndex);
}

void SceneGpuUpdater::refreshAfterLoad(Scene& scene, uint32_t frameIndex)
{
    scene.RefreshSceneWorld(frameIndex);
    scene.PublishRenderSnapshot(frameIndex);

    SceneGpuResources& gpu = scene.GetGpuResources();
    if (!gpu.device->waitForIdle())
        return;

    nvrhi::CommandListHandle commandList = gpu.device->createCommandList();
    commandList->open();

    EnsureMeshGpuBuffers(scene, commandList);
    const GpuReadFrameScope gpuReadScope(scene, frameIndex);
    UpdateGpuSceneBuffers(scene, commandList, frameIndex, /*structureChanged=*/true, /*transformsChanged=*/true);

    commandList->close();
    gpu.device->executeCommandList(commandList);
}

} // namespace caustica::render

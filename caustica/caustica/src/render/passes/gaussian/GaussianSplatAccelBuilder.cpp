#include <render/passes/gaussian/GaussianSplatAccelBuilder.h>

#include <render/passes/gaussian/GaussianSplatGeometry.h>
#include <rhi/utils.h>

#include <cstring>

using namespace caustica::math;

namespace caustica::render
{

GaussianSplatAccelBuilder::GaussianSplatAccelBuilder(nvrhi::IDevice* device)
    : m_device(device)
{
}

void GaussianSplatAccelBuilder::invalidate()
{
    m_buildPending = true;
}

void GaussianSplatAccelBuilder::release(bool markBuildPending)
{
    m_bottomLevelAS = nullptr;
    m_topLevelAS = nullptr;
    m_triangleVertexBuffer = nullptr;
    m_triangleIndexBuffer = nullptr;
    m_buildPending = markBuildPending;
}

void GaussianSplatAccelBuilder::build(
    nvrhi::ICommandList* commandList,
    const GaussianSplatAccelBuildParams& params,
    const std::vector<caustica::GaussianSplatData>& splats,
    uint32_t splatCount,
    nvrhi::IBuffer* aabbBuffer)
{
    if (splatCount == 0 || aabbBuffer == nullptr)
        return;

    if (!m_buildPending
        && m_lastBlasCompaction == params.allowBlasCompaction
        && m_lastUseAABBs == params.useAABBs
        && m_lastUseTLASInstances == params.useTLASInstances
        && std::abs(m_lastSplatScale - params.splatScale) < 1e-4f
        && m_lastKernelDegree == params.kernelDegree
        && m_lastAdaptiveClamp == params.adaptiveClamp
        && m_bottomLevelAS
        && m_topLevelAS)
    {
        return;
    }

    nvrhi::rt::GeometryDesc geometryDesc;
    m_shadowPrimitiveCountPerSplat = params.useAABBs
        ? 1u
        : uint32_t(kGaussianSplatUnitIcosahedronIndices.size() / 3u);

    if (params.useAABBs)
    {
        std::vector<nvrhi::rt::GeometryAABB> aabbs;
        if (params.useTLASInstances)
        {
            aabbs.push_back({ -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f });
        }
        else
        {
            aabbs = buildGaussianAabbs(splats, params.splatScale, params.kernelDegree, params.adaptiveClamp);
        }

        commandList->writeBuffer(aabbBuffer, aabbs.data(), aabbs.size() * sizeof(nvrhi::rt::GeometryAABB));
        commandList->setBufferState(aabbBuffer, nvrhi::ResourceStates::AccelStructBuildInput);
        commandList->commitBarriers();

        nvrhi::rt::GeometryAABBs aabbGeometry;
        aabbGeometry.buffer = aabbBuffer;
        aabbGeometry.offset = 0;
        aabbGeometry.count = uint32_t(aabbs.size());
        aabbGeometry.stride = sizeof(nvrhi::rt::GeometryAABB);
        geometryDesc.setAABBs(aabbGeometry);
        geometryDesc.flags = nvrhi::rt::GeometryFlags::NoDuplicateAnyHitInvocation;
    }
    else
    {
        std::vector<float3> vertices;
        std::vector<uint32_t> indices;
        if (params.useTLASInstances)
        {
            vertices.assign(kGaussianSplatUnitIcosahedronVertices.begin(), kGaussianSplatUnitIcosahedronVertices.end());
            indices.assign(kGaussianSplatUnitIcosahedronIndices.begin(), kGaussianSplatUnitIcosahedronIndices.end());
        }
        else
        {
            vertices.reserve(size_t(splatCount) * kGaussianSplatUnitIcosahedronVertices.size());
            indices.reserve(size_t(splatCount) * kGaussianSplatUnitIcosahedronIndices.size());
            for (uint32_t splatIndex = 0; splatIndex < splatCount; ++splatIndex)
            {
                const caustica::GaussianSplatData& splat = splats[splatIndex];
                const float3 center = splat.centerOpacity.xyz();
                const float3 extent = gaussianAabbExtent(splat, params.splatScale, params.kernelDegree, params.adaptiveClamp);
                const uint32_t vertexBase = uint32_t(vertices.size());
                for (const caustica::math::float3& unitVertex : kGaussianSplatUnitIcosahedronVertices)
                    vertices.push_back(center + unitVertex * extent);
                for (uint32_t index : kGaussianSplatUnitIcosahedronIndices)
                    indices.push_back(vertexBase + index);
            }
        }

        nvrhi::BufferDesc vertexDesc;
        vertexDesc.byteSize = uint64_t(vertices.size()) * sizeof(float3);
        vertexDesc.structStride = sizeof(float3);
        vertexDesc.format = nvrhi::Format::RGB32_FLOAT;
        vertexDesc.isVertexBuffer = true;
        vertexDesc.isAccelStructBuildInput = true;
        vertexDesc.initialState = nvrhi::ResourceStates::AccelStructBuildInput;
        vertexDesc.keepInitialState = true;
        vertexDesc.debugName = "GaussianSplatIcosahedronVertexBuffer";
        m_triangleVertexBuffer = m_device->createBuffer(vertexDesc);

        nvrhi::BufferDesc indexDesc;
        indexDesc.byteSize = uint64_t(indices.size()) * sizeof(uint32_t);
        indexDesc.format = nvrhi::Format::R32_UINT;
        indexDesc.isIndexBuffer = true;
        indexDesc.isAccelStructBuildInput = true;
        indexDesc.initialState = nvrhi::ResourceStates::AccelStructBuildInput;
        indexDesc.keepInitialState = true;
        indexDesc.debugName = "GaussianSplatIcosahedronIndexBuffer";
        m_triangleIndexBuffer = m_device->createBuffer(indexDesc);

        commandList->writeBuffer(m_triangleVertexBuffer, vertices.data(), vertices.size() * sizeof(float3));
        commandList->writeBuffer(m_triangleIndexBuffer, indices.data(), indices.size() * sizeof(uint32_t));
        commandList->setBufferState(m_triangleVertexBuffer, nvrhi::ResourceStates::AccelStructBuildInput);
        commandList->setBufferState(m_triangleIndexBuffer, nvrhi::ResourceStates::AccelStructBuildInput);
        commandList->commitBarriers();

        nvrhi::rt::GeometryTriangles triangles;
        triangles.vertexBuffer = m_triangleVertexBuffer;
        triangles.indexBuffer = m_triangleIndexBuffer;
        triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
        triangles.indexFormat = nvrhi::Format::R32_UINT;
        triangles.vertexStride = sizeof(float3);
        triangles.vertexCount = uint32_t(vertices.size());
        triangles.indexCount = uint32_t(indices.size());
        geometryDesc.setTriangles(triangles);
        geometryDesc.flags = nvrhi::rt::GeometryFlags::None;
    }

    nvrhi::rt::AccelStructDesc blasDesc;
    blasDesc.isTopLevel = false;
    blasDesc.debugName = params.useAABBs ? "GaussianSplatAabbBLAS" : "GaussianSplatIcosahedronBLAS";
    blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace
        | (params.allowBlasCompaction
            ? nvrhi::rt::AccelStructBuildFlags::AllowCompaction
            : nvrhi::rt::AccelStructBuildFlags::AllowUpdate);
    blasDesc.bottomLevelGeometries.push_back(geometryDesc);

    m_bottomLevelAS = m_device->createAccelStruct(blasDesc);
    nvrhi::utils::BuildBottomLevelAccelStruct(commandList, m_bottomLevelAS, blasDesc);

    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.isTopLevel = true;
    tlasDesc.debugName = "GaussianSplatTLAS";
    tlasDesc.topLevelMaxInstances = params.useTLASInstances ? splatCount : 1;
    tlasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace | nvrhi::rt::AccelStructBuildFlags::AllowUpdate;
    m_topLevelAS = m_device->createAccelStruct(tlasDesc);

    std::vector<nvrhi::rt::InstanceDesc> instances;
    instances.resize(params.useTLASInstances ? splatCount : 1u);
    if (params.useTLASInstances)
    {
        for (uint32_t splatIndex = 0; splatIndex < splatCount; ++splatIndex)
        {
            const caustica::GaussianSplatData& splat = splats[splatIndex];
            nvrhi::rt::InstanceDesc& instanceDesc = instances[splatIndex];
            instanceDesc.bottomLevelAS = m_bottomLevelAS;
            instanceDesc.instanceMask = 0xff;
            instanceDesc.instanceID = splatIndex;
            instanceDesc.instanceContributionToHitGroupIndex = 0;
            instanceDesc.flags = nvrhi::rt::InstanceFlags::ForceNonOpaque;
            fillScaleTranslateTransform(
                instanceDesc.transform,
                splat.centerOpacity.xyz(),
                gaussianAabbExtent(splat, params.splatScale, params.kernelDegree, params.adaptiveClamp));
        }
    }
    else
    {
        nvrhi::rt::InstanceDesc& instanceDesc = instances[0];
        instanceDesc.bottomLevelAS = m_bottomLevelAS;
        instanceDesc.instanceMask = 0xff;
        instanceDesc.instanceID = 0;
        instanceDesc.instanceContributionToHitGroupIndex = 0;
        instanceDesc.flags = nvrhi::rt::InstanceFlags::ForceNonOpaque;
        std::memcpy(instanceDesc.transform, nvrhi::rt::c_IdentityTransform, sizeof(nvrhi::rt::AffineTransform));
    }

    commandList->buildTopLevelAccelStruct(
        m_topLevelAS,
        instances.data(),
        instances.size(),
        nvrhi::rt::AccelStructBuildFlags::PreferFastTrace | nvrhi::rt::AccelStructBuildFlags::AllowUpdate);

    m_buildPending = false;
    m_lastBlasCompaction = params.allowBlasCompaction;
    m_lastUseAABBs = params.useAABBs;
    m_lastUseTLASInstances = params.useTLASInstances;
    m_lastSplatScale = params.splatScale;
    m_lastKernelDegree = params.kernelDegree;
    m_lastAdaptiveClamp = params.adaptiveClamp;
}

} // namespace caustica::render

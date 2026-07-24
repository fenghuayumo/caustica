#include <render/passes/gaussian/GaussianSplatAccelBuilder.h>

#include <render/passes/gaussian/GaussianSplatGeometry.h>
#include <rhi/utils.h>

#include <cstring>

using namespace caustica::math;

namespace caustica::render
{

GaussianSplatAccelBuilder::GaussianSplatAccelBuilder(caustica::rhi::Device* device)
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
    caustica::rhi::CommandList* commandList,
    const GaussianSplatAccelBuildParams& params,
    const std::vector<caustica::GaussianSplatData>& splats,
    uint32_t splatCount,
    caustica::rhi::Buffer* aabbBuffer)
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

    caustica::rhi::rt::GeometryDesc geometryDesc;
    m_shadowPrimitiveCountPerSplat = params.useAABBs
        ? 1u
        : uint32_t(kGaussianSplatUnitIcosahedronIndices.size() / 3u);

    if (params.useAABBs)
    {
        std::vector<caustica::rhi::rt::GeometryAABB> aabbs;
        if (params.useTLASInstances)
        {
            aabbs.push_back({ -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f });
        }
        else
        {
            aabbs = buildGaussianAabbs(splats, params.splatScale, params.kernelDegree, params.adaptiveClamp);
        }

        commandList->writeBuffer(aabbBuffer, aabbs.data(), aabbs.size() * sizeof(caustica::rhi::rt::GeometryAABB));
        commandList->setBufferState(aabbBuffer, caustica::rhi::ResourceStates::AccelStructBuildInput);
        commandList->commitBarriers();

        caustica::rhi::rt::GeometryAABBs aabbGeometry;
        aabbGeometry.buffer = aabbBuffer;
        aabbGeometry.offset = 0;
        aabbGeometry.count = uint32_t(aabbs.size());
        aabbGeometry.stride = sizeof(caustica::rhi::rt::GeometryAABB);
        geometryDesc.setAABBs(aabbGeometry);
        geometryDesc.flags = caustica::rhi::rt::GeometryFlags::NoDuplicateAnyHitInvocation;
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

        caustica::rhi::BufferDesc vertexDesc;
        vertexDesc.byteSize = uint64_t(vertices.size()) * sizeof(float3);
        vertexDesc.structStride = sizeof(float3);
        vertexDesc.format = caustica::rhi::Format::RGB32_FLOAT;
        vertexDesc.isVertexBuffer = true;
        vertexDesc.isAccelStructBuildInput = true;
        vertexDesc.initialState = caustica::rhi::ResourceStates::AccelStructBuildInput;
        vertexDesc.keepInitialState = true;
        vertexDesc.debugName = "GaussianSplatIcosahedronVertexBuffer";
        m_triangleVertexBuffer = m_device->createBuffer(vertexDesc);

        caustica::rhi::BufferDesc indexDesc;
        indexDesc.byteSize = uint64_t(indices.size()) * sizeof(uint32_t);
        indexDesc.format = caustica::rhi::Format::R32_UINT;
        indexDesc.isIndexBuffer = true;
        indexDesc.isAccelStructBuildInput = true;
        indexDesc.initialState = caustica::rhi::ResourceStates::AccelStructBuildInput;
        indexDesc.keepInitialState = true;
        indexDesc.debugName = "GaussianSplatIcosahedronIndexBuffer";
        m_triangleIndexBuffer = m_device->createBuffer(indexDesc);

        commandList->writeBuffer(m_triangleVertexBuffer, vertices.data(), vertices.size() * sizeof(float3));
        commandList->writeBuffer(m_triangleIndexBuffer, indices.data(), indices.size() * sizeof(uint32_t));
        commandList->setBufferState(m_triangleVertexBuffer, caustica::rhi::ResourceStates::AccelStructBuildInput);
        commandList->setBufferState(m_triangleIndexBuffer, caustica::rhi::ResourceStates::AccelStructBuildInput);
        commandList->commitBarriers();

        caustica::rhi::rt::GeometryTriangles triangles;
        triangles.vertexBuffer = m_triangleVertexBuffer;
        triangles.indexBuffer = m_triangleIndexBuffer;
        triangles.vertexFormat = caustica::rhi::Format::RGB32_FLOAT;
        triangles.indexFormat = caustica::rhi::Format::R32_UINT;
        triangles.vertexStride = sizeof(float3);
        triangles.vertexCount = uint32_t(vertices.size());
        triangles.indexCount = uint32_t(indices.size());
        geometryDesc.setTriangles(triangles);
        geometryDesc.flags = caustica::rhi::rt::GeometryFlags::None;
    }

    caustica::rhi::rt::AccelStructDesc blasDesc;
    blasDesc.isTopLevel = false;
    blasDesc.debugName = params.useAABBs ? "GaussianSplatAabbBLAS" : "GaussianSplatIcosahedronBLAS";
    blasDesc.buildFlags = caustica::rhi::rt::AccelStructBuildFlags::PreferFastTrace
        | (params.allowBlasCompaction
            ? caustica::rhi::rt::AccelStructBuildFlags::AllowCompaction
            : caustica::rhi::rt::AccelStructBuildFlags::AllowUpdate);
    blasDesc.bottomLevelGeometries.push_back(geometryDesc);

    m_bottomLevelAS = m_device->createAccelStruct(blasDesc);
    caustica::rhi::utils::BuildBottomLevelAccelStruct(commandList, m_bottomLevelAS, blasDesc);

    caustica::rhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.isTopLevel = true;
    tlasDesc.debugName = "GaussianSplatTLAS";
    tlasDesc.topLevelMaxInstances = params.useTLASInstances ? splatCount : 1;
    tlasDesc.buildFlags = caustica::rhi::rt::AccelStructBuildFlags::PreferFastTrace | caustica::rhi::rt::AccelStructBuildFlags::AllowUpdate;
    m_topLevelAS = m_device->createAccelStruct(tlasDesc);

    std::vector<caustica::rhi::rt::InstanceDesc> instances;
    instances.resize(params.useTLASInstances ? splatCount : 1u);
    if (params.useTLASInstances)
    {
        for (uint32_t splatIndex = 0; splatIndex < splatCount; ++splatIndex)
        {
            const caustica::GaussianSplatData& splat = splats[splatIndex];
            caustica::rhi::rt::InstanceDesc& instanceDesc = instances[splatIndex];
            instanceDesc.bottomLevelAS = m_bottomLevelAS;
            instanceDesc.instanceMask = 0xff;
            instanceDesc.instanceID = splatIndex;
            instanceDesc.instanceContributionToHitGroupIndex = 0;
            instanceDesc.flags = caustica::rhi::rt::InstanceFlags::ForceNonOpaque;
            fillScaleTranslateTransform(
                instanceDesc.transform,
                splat.centerOpacity.xyz(),
                gaussianAabbExtent(splat, params.splatScale, params.kernelDegree, params.adaptiveClamp));
        }
    }
    else
    {
        caustica::rhi::rt::InstanceDesc& instanceDesc = instances[0];
        instanceDesc.bottomLevelAS = m_bottomLevelAS;
        instanceDesc.instanceMask = 0xff;
        instanceDesc.instanceID = 0;
        instanceDesc.instanceContributionToHitGroupIndex = 0;
        instanceDesc.flags = caustica::rhi::rt::InstanceFlags::ForceNonOpaque;
        std::memcpy(instanceDesc.transform, caustica::rhi::rt::c_IdentityTransform, sizeof(caustica::rhi::rt::AffineTransform));
    }

    commandList->buildTopLevelAccelStruct(
        m_topLevelAS,
        instances.data(),
        instances.size(),
        caustica::rhi::rt::AccelStructBuildFlags::PreferFastTrace | caustica::rhi::rt::AccelStructBuildFlags::AllowUpdate);

    m_buildPending = false;
    m_lastBlasCompaction = params.allowBlasCompaction;
    m_lastUseAABBs = params.useAABBs;
    m_lastUseTLASInstances = params.useTLASInstances;
    m_lastSplatScale = params.splatScale;
    m_lastKernelDegree = params.kernelDegree;
    m_lastAdaptiveClamp = params.adaptiveClamp;
}

} // namespace caustica::render

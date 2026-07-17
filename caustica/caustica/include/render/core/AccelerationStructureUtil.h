#pragma once

#include <core/log.h>
#include <scene/SceneTypes.h>
#include <rhi/nvrhi.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/SceneGpuResources.h>

namespace bvh
{
    struct Config
    {
        bool excludeTransmissive = false;
    };

    struct OmmAttachment
    {
        nvrhi::rt::OpacityMicromapHandle ommBuffer;
        nvrhi::Format ommIndexFormat = nvrhi::Format::UNKNOWN;
        std::vector<nvrhi::rt::OpacityMicromapUsageCount> ommIndexHistogram;
        nvrhi::BufferHandle ommIndexBuffer;
        uint32_t ommIndexBufferOffset = 0;
        nvrhi::BufferHandle ommArrayDataBuffer;
        uint32_t ommArrayDataBufferOffset = 0;
    };

    static bool isValidTriangleGeometryForBlas(
        const caustica::MeshInfo& mesh,
        const caustica::render::MeshGpuRecord& meshGpu,
        const caustica::MeshGeometry& geometry)
    {
        if (geometry.type != caustica::MeshGeometryPrimitiveType::Triangles)
            return false;

        if (geometry.numIndices == 0 || geometry.numVertices == 0)
            return false;

        if ((geometry.numIndices % 3) != 0)
        {
            caustica::warning("Skipping BLAS geometry '%s': index count %u is not divisible by 3.",
                mesh.name.c_str(), geometry.numIndices);
            return false;
        }

        if (!mesh.buffers || !meshGpu.indexBuffer || !meshGpu.vertexBuffer)
            return false;

        const nvrhi::BufferDesc& indexBufferDesc = meshGpu.indexBuffer->getDesc();
        const nvrhi::BufferDesc& vertexBufferDesc = meshGpu.vertexBuffer->getDesc();
        const nvrhi::BufferRange& positionRange =
            meshGpu.vertexBufferRange(caustica::VertexAttribute::Position);

        if (positionRange.byteSize == 0)
            return false;

        const uint64_t indexStart = uint64_t(mesh.indexOffset + geometry.indexOffsetInMesh) * sizeof(uint32_t);
        const uint64_t indexEnd = indexStart + uint64_t(geometry.numIndices) * sizeof(uint32_t);
        if (indexEnd > indexBufferDesc.byteSize)
        {
            caustica::warning("Skipping BLAS geometry '%s': index range [%llu, %llu) exceeds index buffer size %llu.",
                mesh.name.c_str(),
                static_cast<unsigned long long>(indexStart),
                static_cast<unsigned long long>(indexEnd),
                static_cast<unsigned long long>(indexBufferDesc.byteSize));
            return false;
        }

        const uint64_t vertexStart = positionRange.byteOffset
            + uint64_t(mesh.vertexOffset + geometry.vertexOffsetInMesh) * sizeof(dm::float3);
        const uint64_t vertexEnd = vertexStart + uint64_t(geometry.numVertices) * sizeof(dm::float3);
        const uint64_t positionRangeEnd = positionRange.byteOffset + positionRange.byteSize;
        if (vertexEnd > vertexBufferDesc.byteSize || vertexEnd > positionRangeEnd)
        {
            caustica::warning("Skipping BLAS geometry '%s': position range [%llu, %llu) exceeds vertex buffer size %llu or position range end %llu.",
                mesh.name.c_str(),
                static_cast<unsigned long long>(vertexStart),
                static_cast<unsigned long long>(vertexEnd),
                static_cast<unsigned long long>(vertexBufferDesc.byteSize),
                static_cast<unsigned long long>(positionRangeEnd));
            return false;
        }

        if (!mesh.buffers->indexData.empty())
        {
            const uint64_t indexDataStart = uint64_t(mesh.indexOffset + geometry.indexOffsetInMesh);
            const uint64_t indexDataEnd = indexDataStart + geometry.numIndices;
            if (indexDataEnd <= mesh.buffers->indexData.size())
            {
                uint32_t maxIndex = 0;
                for (uint64_t indexIt = indexDataStart; indexIt < indexDataEnd; ++indexIt)
                    maxIndex = std::max(maxIndex, mesh.buffers->indexData[size_t(indexIt)]);

                if (maxIndex >= geometry.numVertices)
                {
                    caustica::warning("Skipping BLAS geometry '%s': max index %u exceeds geometry vertex count %u.",
                        mesh.name.c_str(), maxIndex, geometry.numVertices);
                    return false;
                }
            }
        }

        return true;
    }

    static nvrhi::rt::AccelStructDesc getMeshBlasDesc(
        const Config& cfg,
        const caustica::MeshInfo& mesh,
        const caustica::render::MeshGpuRecord& meshGpu,
        const OmmAttachment* ommAttachment,
        bool updateSkinMeshes)
    {
        nvrhi::rt::AccelStructDesc blasDesc;
        blasDesc.isTopLevel = false;
        blasDesc.debugName = mesh.name;

        if (!mesh.buffers || !meshGpu.indexBuffer || !meshGpu.vertexBuffer)
            return blasDesc;

        for (uint32_t geomIt = 0; geomIt < mesh.geometries.size(); ++geomIt)
        {
            const caustica::MeshGeometry* geometry = mesh.geometries[geomIt].get();
            if (!geometry)
                continue;
            if (!isValidTriangleGeometryForBlas(mesh, meshGpu, *geometry))
                continue;

            nvrhi::rt::GeometryDesc geometryDesc;
            auto& triangles = geometryDesc.geometryData.triangles;
            triangles.indexBuffer = meshGpu.indexBuffer;
            triangles.indexOffset = (mesh.indexOffset + geometry->indexOffsetInMesh) * sizeof(uint32_t);
            triangles.indexFormat = nvrhi::Format::R32_UINT;
            triangles.indexCount = geometry->numIndices;
            triangles.vertexBuffer = meshGpu.vertexBuffer;
            triangles.vertexOffset = (mesh.vertexOffset + geometry->vertexOffsetInMesh) * sizeof(dm::float3)
                + meshGpu.vertexBufferRange(caustica::VertexAttribute::Position).byteOffset;
            triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
            triangles.vertexStride = sizeof(dm::float3);
            triangles.vertexCount = geometry->numVertices;

            std::shared_ptr<PTMaterial> materialPT = PTMaterial::safeCast(geometry->material);
            const bool skipRender = materialPT && materialPT->skipRender;
            const bool isTransmissive = geometry->material && geometry->material->domain == caustica::MaterialDomain::Transmissive;

            if ((cfg.excludeTransmissive && isTransmissive) || skipRender)
            {
                continue;
            }

            if (ommAttachment)
            {
                const OmmAttachment& omm = ommAttachment[geomIt];
                triangles.opacityMicromap = omm.ommBuffer;
                triangles.ommIndexBuffer = omm.ommIndexBuffer;
                triangles.ommIndexBufferOffset = omm.ommIndexBufferOffset;
                triangles.ommIndexFormat = omm.ommIndexFormat;
                triangles.pOmmUsageCounts = omm.ommIndexHistogram.data();
                triangles.numOmmUsageCounts = (uint32_t)omm.ommIndexHistogram.size();
            }

            geometryDesc.geometryType = nvrhi::rt::GeometryType::Triangles;
            
            // Alpha testing, NEE exclusion, and transparent shadow tinting require custom shader handling.
            const bool needsCustomShader =
                materialPT && (materialPT->enableAlphaTesting || materialPT->excludeFromNEE || materialPT->enableTransmission);
            geometryDesc.flags = needsCustomShader ? nvrhi::rt::GeometryFlags::None : nvrhi::rt::GeometryFlags::Opaque;
            blasDesc.bottomLevelGeometries.push_back(geometryDesc);
        }

        // Skinned meshes and fixed-topology point caches (USD GeometrySequence /
        // DeformationSourcePositionIndices) are updated every frame. Build them with
        // AllowUpdate up front so the first animation tick can PerformUpdate in-place
        // instead of allocating a second BLAS (VRAM spike / TDR on large imports).
        const bool needsDynamicUpdate = mesh.skinPrototype.use_count() != 0
            || !mesh.DeformationSourcePositionIndices.empty();
        if (needsDynamicUpdate)
        {
            const nvrhi::rt::AccelStructBuildFlags quality =
                !mesh.DeformationSourcePositionIndices.empty()
                    ? nvrhi::rt::AccelStructBuildFlags::PreferFastBuild
                    : nvrhi::rt::AccelStructBuildFlags::PreferFastTrace;
            blasDesc.buildFlags = quality
                | (updateSkinMeshes
                    ? nvrhi::rt::AccelStructBuildFlags::PerformUpdate
                    : nvrhi::rt::AccelStructBuildFlags::AllowUpdate);
        }
        else
        {
            blasDesc.buildFlags = nvrhi::rt::AccelStructBuildFlags::PreferFastTrace; // | nvrhi::rt::AccelStructBuildFlags::MinimizeMemory;
        }

        return blasDesc;
    }
} // namespace util

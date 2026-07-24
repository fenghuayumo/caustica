#pragma once

#include <rhi/rhi.h>

namespace rtxdi
{
    class ReSTIRDIContext;
    class ReSTIRPTContext;
    class RISBufferSegmentAllocator;
}

class RtxdiResources
{
private:
    bool m_NeighborOffsetsInitialized = false;
    uint32_t m_MaxEmissiveMeshes = 0;
    uint32_t m_MaxEmissiveTriangles = 0;
    uint32_t m_MaxPrimitiveLights = 0;
    uint32_t m_MaxGeometryInstances = 0;

public:
    caustica::rhi::BufferHandle TaskBuffer;
    caustica::rhi::BufferHandle PrimitiveLightBuffer;
    caustica::rhi::BufferHandle LightDataBuffer;
    caustica::rhi::BufferHandle GeometryInstanceToLightBuffer;
    caustica::rhi::BufferHandle LightIndexMappingBuffer;
    caustica::rhi::BufferHandle RisBuffer;
    caustica::rhi::BufferHandle RisLightDataBuffer;
    caustica::rhi::BufferHandle NeighborOffsetsBuffer;
    caustica::rhi::BufferHandle LightReservoirBuffer;
    caustica::rhi::BufferHandle GIReservoirBuffer;
    caustica::rhi::BufferHandle PTReservoirBuffer;
    caustica::rhi::TextureHandle LocalLightPdfTexture;

    RtxdiResources(
        caustica::rhi::Device* device, 
        const rtxdi::ReSTIRDIContext& context,
        const rtxdi::ReSTIRPTContext& ptContext,
        const rtxdi::RISBufferSegmentAllocator& risBufferSegmentAllocator,
        uint32_t maxEmissiveMeshes,
        uint32_t maxEmissiveTriangles,
        uint32_t maxPrimitiveLights,
        uint32_t maxGeometryInstances);

    void initializeNeighborOffsets(caustica::rhi::CommandList* commandList, uint32_t neighborOffsetCount);

    uint32_t getMaxEmissiveMeshes() const { return m_MaxEmissiveMeshes; }
    uint32_t getMaxEmissiveTriangles() const { return m_MaxEmissiveTriangles; }
    uint32_t getMaxPrimitiveLights() const { return m_MaxPrimitiveLights; }
    uint32_t getMaxGeometryInstances() const { return m_MaxGeometryInstances; }

    caustica::rhi::BufferHandle getRisLightDataBuffer() const { return RisLightDataBuffer; }
    caustica::rhi::BufferHandle getLightDataBuffer() const { return LightDataBuffer; }
    caustica::rhi::BufferHandle getRisBuffer() const { return RisBuffer; }

    static constexpr uint32_t c_NumReservoirBuffers = 3;
    static constexpr uint32_t c_NumGIReservoirBuffers = 2;
    static constexpr uint32_t c_NumPTReservoirBuffers = 2;

};

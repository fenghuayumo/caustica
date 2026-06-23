#pragma pack_matrix(row_major)

#define NON_PATH_TRACING_PASS 1
#define RTXDI_RESTIR_PT_HYBRID_SHIFT

#include "../RTXDI/PTPathTracer.hlsli"
#include <Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli>
#include <Rtxdi/PT/SpatialResampling.hlsli>

[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID)
{
    const uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(globalIndex, g_RtxdiBridgeConst.runtimeParams.activeCheckerboardField);
    const uint2 reservoirPosition = RTXDI_PixelPosToReservoirPos(pixelPosition, g_RtxdiBridgeConst.runtimeParams.activeCheckerboardField);

    RTXDI_PTSpatialResamplingRuntimeParameters runtimeParams = RTXDI_EmptyPTSpatialResamplingRuntimeParameters();
    runtimeParams.PixelPosition = pixelPosition;
    runtimeParams.ReservoirPosition = reservoirPosition;
    runtimeParams.cameraPos = g_Const.ptConsts.camera.PosW;
    runtimeParams.prevCameraPos = g_Const.ptConsts.prevCamera.PosW;
    runtimeParams.prevPrevCameraPos = g_Const.ptConsts.prevCamera.PosW;

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(
        pixelPosition,
        g_RtxdiBridgeConst.frameIndex,
        RTXDI_PT_SPATIAL_RESAMPLING_RANDOM_SEED);

    RAB_PathTracerUserData userData = RAB_EmptyPathTracerUserData();
    bool selectedNeighborSample = false;

    RTXDI_PTReservoir reservoir = RTXDI_PTSpatialResampling(
        runtimeParams,
        g_RtxdiBridgeConst.restirPT.spatialResampling,
        g_RtxdiBridgeConst.restirPT.hybridShift,
        g_RtxdiBridgeConst.restirPT.reconnection,
        g_RtxdiBridgeConst.restirPT.reservoirBuffer,
        g_RtxdiBridgeConst.restirPT.bufferIndices,
        g_RtxdiBridgeConst.runtimeParams,
        rng,
        selectedNeighborSample,
        userData);

    RTXDI_StorePTReservoir(
        reservoir,
        g_RtxdiBridgeConst.restirPT.reservoirBuffer,
        reservoirPosition,
        g_RtxdiBridgeConst.restirPT.bufferIndices.spatialResamplingOutputBufferIndex);
}

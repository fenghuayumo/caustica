#pragma pack_matrix(row_major)

#define NON_PATH_TRACING_PASS 1
#define RTXDI_RESTIR_PT_HYBRID_SHIFT

#if USE_RAY_QUERY
#define RTXDI_ENABLE_BOILING_FILTER
#define RTXDI_BOILING_FILTER_GROUP_SIZE RTXDI_SCREEN_SPACE_GROUP_SIZE
#endif

#include "../RTXDI/PTPathTracer.hlsli"
#include <Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli>
#include <Rtxdi/PT/BoilingFilter.hlsli>
#include <Rtxdi/PT/TemporalResampling.hlsli>

float3 CAUSTICA_RtxdiPT_LoadMotionVector(uint2 pixelPosition)
{
    float3 motionVector = u_MotionVectors[pixelPosition].xyz;
    return convertMotionVectorToPixelSpace(g_Const.view, g_Const.previousView, pixelPosition, motionVector);
}

[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID, uint2 localIndex : SV_GroupThreadID)
{
    RTXDI_PTTemporalResamplingRuntimeParameters runtimeParams = RTXDI_EmptyPTTemporalResamplingRuntimeParameters();
    runtimeParams.pixelPosition = RTXDI_ReservoirPosToPixelPos(globalIndex, g_RtxdiBridgeConst.runtimeParams.activeCheckerboardField);
    runtimeParams.reservoirPosition = RTXDI_PixelPosToReservoirPos(runtimeParams.pixelPosition, g_RtxdiBridgeConst.runtimeParams.activeCheckerboardField);
    runtimeParams.motionVector = CAUSTICA_RtxdiPT_LoadMotionVector(runtimeParams.pixelPosition);
    runtimeParams.cameraPos = g_Const.ptConsts.camera.PosW;
    runtimeParams.prevCameraPos = g_Const.ptConsts.prevCamera.PosW;
    runtimeParams.prevPrevCameraPos = g_Const.ptConsts.prevCamera.PosW;

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(
        runtimeParams.pixelPosition,
        g_RtxdiBridgeConst.frameIndex,
        RTXDI_PT_TEMPORAL_RESAMPLING_RANDOM_SEED);

    RAB_PathTracerUserData userData = RAB_EmptyPathTracerUserData();
    bool selectedPrevSample = false;

    RTXDI_PTReservoir reservoir = RTXDI_PTTemporalResampling(
        g_RtxdiBridgeConst.restirPT.temporalResampling,
        runtimeParams,
        g_RtxdiBridgeConst.restirPT.hybridShift,
        g_RtxdiBridgeConst.restirPT.reconnection,
        g_RtxdiBridgeConst.runtimeParams,
        g_RtxdiBridgeConst.restirPT.reservoirBuffer,
        rng,
        g_RtxdiBridgeConst.restirPT.bufferIndices,
        selectedPrevSample,
        userData);

#ifdef RTXDI_ENABLE_BOILING_FILTER
    if (g_RtxdiBridgeConst.restirPT.boilingFilter.enableBoilingFilter)
    {
        RTXDI_PTBoilingFilter(localIndex, g_RtxdiBridgeConst.restirPT.boilingFilter.boilingFilterStrength, reservoir);
    }
#endif

    RTXDI_StorePTReservoir(
        reservoir,
        g_RtxdiBridgeConst.restirPT.reservoirBuffer,
        runtimeParams.reservoirPosition,
        g_RtxdiBridgeConst.restirPT.bufferIndices.temporalResamplingOutputBufferIndex);
}

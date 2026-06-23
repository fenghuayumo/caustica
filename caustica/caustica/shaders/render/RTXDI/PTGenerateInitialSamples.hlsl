#pragma pack_matrix(row_major)

#define NON_PATH_TRACING_PASS 1
#define RTXDI_RESTIR_PT_INITIAL_SAMPLING

#include "../RTXDI/PTPathTracer.hlsli"
#include <Rtxdi/Utils/RandomSamplerPerPassSeeds.hlsli>

[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID)
{
    const uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(globalIndex, g_RtxdiBridgeConst.runtimeParams.activeCheckerboardField);
    const uint2 reservoirPosition = RTXDI_PixelPosToReservoirPos(pixelPosition, g_RtxdiBridgeConst.runtimeParams.activeCheckerboardField);

    RAB_Surface surface = RAB_GetGBufferSurface(pixelPosition, false);
    if (!RAB_IsSurfaceValid(surface))
    {
        RTXDI_StorePTReservoir(RTXDI_EmptyPTReservoir(), g_RtxdiBridgeConst.restirPT.reservoirBuffer, reservoirPosition, g_RtxdiBridgeConst.restirPT.bufferIndices.initialPathTracerOutputBufferIndex);
        return;
    }

    RTXDI_PathTracerRandomContext ptRandContext = RTXDI_InitializePathTracerRandomContext(
        pixelPosition,
        g_RtxdiBridgeConst.frameIndex,
        RTXDI_PT_GENERATE_INITIAL_SAMPLES_RANDOM_SEED,
        RTXDI_PT_GENERATE_INITIAL_SAMPLES_REPLAY_RANDOM_SEED);

    RAB_PathTracerUserData userData = RAB_EmptyPathTracerUserData();

    RTXDI_PTInitialSamplingRuntimeParameters runtimeParams = RTXDI_EmptyPTInitialSamplingRuntimeParameters();
    runtimeParams.cameraPos = g_Const.ptConsts.camera.PosW;
    runtimeParams.prevCameraPos = g_Const.ptConsts.prevCamera.PosW;
    runtimeParams.prevPrevCameraPos = g_Const.ptConsts.prevCamera.PosW;

    RTXDI_PTReservoir reservoir = GenerateInitialSamples(
        g_RtxdiBridgeConst.restirPT.initialSampling,
        runtimeParams,
        g_RtxdiBridgeConst.restirPT.reconnection,
        ptRandContext,
        surface,
        userData);

    RTXDI_StorePTReservoir(
        reservoir,
        g_RtxdiBridgeConst.restirPT.reservoirBuffer,
        reservoirPosition,
        g_RtxdiBridgeConst.restirPT.bufferIndices.initialPathTracerOutputBufferIndex);
}

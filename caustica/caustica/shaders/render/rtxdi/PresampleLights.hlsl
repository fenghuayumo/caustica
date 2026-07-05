#define NON_PATH_TRACING_PASS 1

#include "RtxdiApplicationBridge.hlsli"
#include <rtxdi/LightSampling/PresamplingFunctions.hlsli>

[numthreads(RTXDI_PRESAMPLING_GROUP_SIZE, 1, 1)] 
void main(uint2 GlobalIndex : SV_DispatchThreadID) 
{
    RAB_RandomSamplerState rng = RAB_InitRandomSampler(GlobalIndex.xy, 0);

#if RTXDI_ENABLE_PRESAMPLING
    RTXDI_PresampleLocalLights(
        rng,
        t_LocalLightPdfTexture,
        g_RtxdiBridgeConst.localLightPdfTextureSize,
        GlobalIndex.y,
        GlobalIndex.x,
        g_RtxdiBridgeConst.lightBufferParams.localLightBufferRegion,
        g_RtxdiBridgeConst.localLightsRISBufferSegmentParams);
#endif
}
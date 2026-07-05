#pragma pack_matrix(row_major)

#define NON_PATH_TRACING_PASS 1

#include "RtxdiApplicationBridge.hlsli"

#include <Rtxdi/ReGIR/ReGIRSampling.hlsli>
#include <rtxdi/LightSampling/PresamplingFunctions.hlsli>

[numthreads(256, 1, 1)]
void main(uint GlobalIndex : SV_DispatchThreadID)
{
    RAB_RandomSamplerState rng = RAB_InitRandomSampler(uint2(GlobalIndex & 0xfff, GlobalIndex >> 12), 1);
    RAB_RandomSamplerState coherentRng = RAB_InitRandomSampler(uint2(GlobalIndex >> 8, 0), 1);

    RTXDI_PresampleLocalLightsForReGIR(
        rng,
        coherentRng,
        GlobalIndex,
        g_RtxdiBridgeConst.lightBufferParams.localLightBufferRegion,
        g_RtxdiBridgeConst.localLightsRISBufferSegmentParams,
        g_RtxdiBridgeConst.regir);
}
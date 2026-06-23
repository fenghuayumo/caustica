#pragma pack_matrix(row_major)

#define NON_PATH_TRACING_PASS 1

#include "../RTXDI/RtxdiApplicationBridge.hlsli"
#include <Rtxdi/PT/Reservoir.hlsli>

float3 RTXPT_RtxdiPT_SanitizeRadiance(float3 radiance)
{
    if (any(isnan(radiance)) || any(isinf(radiance)) || any(radiance < float3(0.0f, 0.0f, 0.0f)))
        return float3(0.0f, 0.0f, 0.0f);

    return radiance;
}

bool ReSTIRPTFinalContribution(uint2 reservoirPosition, uint2 pixelPosition, out float4 radianceAndSpecAvg)
{
    radianceAndSpecAvg = float4(0.0f, 0.0f, 0.0f, 0.0f);

    RAB_Surface surface = RAB_GetGBufferSurface(pixelPosition, false);
    if (!RAB_IsSurfaceValid(surface))
        return false;

    RTXDI_PTReservoir reservoir = RTXDI_LoadPTReservoir(
        g_RtxdiBridgeConst.restirPT.reservoirBuffer,
        reservoirPosition,
        g_RtxdiBridgeConst.restirPT.bufferIndices.finalShadingInputBufferIndex);

    if (!RTXDI_IsValidPTReservoir(reservoir))
        return false;

    float3 pathThroughput = Unpack_R11G11B10_FLOAT(u_Throughput[pixelPosition]);
    float3 radiance = RTXPT_RtxdiPT_SanitizeRadiance(reservoir.TargetFunction * reservoir.WeightSum * pathThroughput);

    radianceAndSpecAvg.rgb = radiance;
    radianceAndSpecAvg.a = Average(radiance);

    return any(radiance > float3(0.0f, 0.0f, 0.0f));
}

[numthreads(RTXDI_SCREEN_SPACE_GROUP_SIZE, RTXDI_SCREEN_SPACE_GROUP_SIZE, 1)]
void main(uint2 globalIndex : SV_DispatchThreadID)
{
    const uint2 pixelPosition = RTXDI_ReservoirPosToPixelPos(globalIndex, g_RtxdiBridgeConst.runtimeParams.activeCheckerboardField);
    const uint2 reservoirPosition = RTXDI_PixelPosToReservoirPos(pixelPosition, g_RtxdiBridgeConst.runtimeParams.activeCheckerboardField);

    float4 radianceAndSpecAvg = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (!ReSTIRPTFinalContribution(reservoirPosition, pixelPosition, radianceAndSpecAvg))
        return;

    if (g_Const.ptConsts.denoisingEnabled)
    {
        uint address = StablePlanesContext::ComputeDominantAddress(pixelPosition, u_StablePlanesHeader, u_StablePlanesBuffer, u_StableRadiance, g_Const.ptConsts);
        float4 radiance = Fp16ToFp32(u_StablePlanesBuffer[address].PackedNoisyRadianceAndSpecAvg);
        u_StablePlanesBuffer[address].PackedNoisyRadianceAndSpecAvg = Fp32ToFp16(radiance + radianceAndSpecAvg);
    }
    else
    {
        u_OutputColor[pixelPosition] += float4(radianceAndSpecAvg.rgb, 0.0f);
    }
}

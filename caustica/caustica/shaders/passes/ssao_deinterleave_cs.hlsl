#pragma pack_matrix(row_major)

#include <shaders/ssao_cb.h>
#include <shaders/binding_helpers.hlsli>

Texture2D<float> t_InputDepth : register(t0);
VK_IMAGE_FORMAT("r32f") RWTexture2DArray<float> u_DeinterleavedDepth : register(u0);

cbuffer c_Ssao : register(b0)
{
    SsaoConstants g_Ssao;
};

[numthreads(8, 8, 1)]
void main(uint3 globalId : SV_DispatchThreadID)
{
    float depths[16];
    uint2 groupBase = globalId.xy * 4 + g_Ssao.quantizedViewportOrigin;

    [unroll] 
    for (uint y = 0; y < 4; y++)
    { 
        [unroll] 
        for (uint x = 0; x < 4; x++)
        {
            uint2 gbufferSamplePos = groupBase + uint2(x, y);
            float depth = t_InputDepth[gbufferSamplePos];

#if LINEAR_DEPTH
            float linearDepth = depth;
#else
            float4 clipPos = float4(0, 0, depth, 1);
            float4 viewPos = mul(clipPos, g_Ssao.view.matClipToView);
            float linearDepth = viewPos.z / viewPos.w;
#endif

            depths[y * 4 + x] = linearDepth;
        }
    }

    uint2 quarterResPos = groupBase >> 2;

    [unroll]
    for(uint index = 0; index < 16; index++)
    {
        float depth = depths[index];
        u_DeinterleavedDepth[uint3(quarterResPos.xy, index)] = depth;
    }
}

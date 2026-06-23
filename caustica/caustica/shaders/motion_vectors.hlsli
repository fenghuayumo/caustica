#ifndef MOTION_VECTORS_HLSLI
#define MOTION_VECTORS_HLSLI

#include "view_cb.h"

float3 GetMotionVector(float3 svPosition, float3 prevWorldPos, PlanarViewConstants view, PlanarViewConstants viewPrev)
{
    float4 clipPos = mul(float4(prevWorldPos, 1), viewPrev.matWorldToClip);
    if (clipPos.w <= 0)
        return 0;

    clipPos.xyz /= clipPos.w;
    float2 prevWindowPos = clipPos.xy * view.clipToWindowScale + view.clipToWindowBias;

    float3 motion;
    motion.xy = prevWindowPos - svPosition.xy + (view.pixelOffset - viewPrev.pixelOffset);
    motion.z = clipPos.z - svPosition.z;
    return motion;
}

#endif
#ifndef SKY_HLSLI
#define SKY_HLSLI

#if !__SLANG__
#pragma pack_matrix(row_major)
#endif

#include "shaders/sky_cb.h"

float3 ProceduralSky(ProceduralSkyShaderParameters params, float3 direction, float angularSizeOfPixel)
{
    float elevation = asin(clamp(dot(direction, params.directionUp), -1.0, 1.0));
    float top = smoothstep(0.f, params.horizonSize, elevation);
    float bottom = smoothstep(0.f, params.horizonSize, -elevation);
    float3 environment = lerp(lerp(params.horizonColor, params.groundColor, bottom), params.skyColor, top);

    float angleToLight = acos(saturate(dot(direction, params.directionToLight)));
    float halfAngularSize = params.angularSizeOfLight * 0.5;
    float lightIntensity = saturate(1.0 - smoothstep(halfAngularSize - angularSizeOfPixel * 2, halfAngularSize + angularSizeOfPixel * 2, angleToLight));
    lightIntensity = pow(lightIntensity, 4.0);
    float glowInput = saturate(2.0 * (1.0 - smoothstep(halfAngularSize - params.glowSize, halfAngularSize + params.glowSize, angleToLight)));
    float glowIntensity = params.glowIntensity * pow(glowInput, params.glowSharpness);
    float3 light = max(lightIntensity, glowIntensity) * params.lightColor;
    
    return environment + light;
}

#endif
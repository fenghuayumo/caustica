#ifndef SKY_CB_H
#define SKY_CB_H

struct ProceduralSkyShaderParameters
{
    float3 directionToLight;
    float angularSizeOfLight;

    float3 lightColor;
    float glowSize;

    float3 skyColor;
    float glowIntensity;

    float3 horizonColor;
    float horizonSize;

    float3 groundColor;
    float glowSharpness;

    float3 directionUp;
    float pad1;
};

struct SkyConstants
{
    float4x4 matClipToTranslatedWorld;

    ProceduralSkyShaderParameters params;
};

#endif // SKY_CB_H
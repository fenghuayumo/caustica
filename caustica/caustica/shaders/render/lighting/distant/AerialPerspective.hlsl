// Depth-aware aerial perspective for the path-traced HDR result.
// Uses the same Hillaire transmittance and multi-scattering LUTs as the sky.

#pragma pack_matrix(row_major)

#include "AerialPerspective.hlsli"
#include "SkyAtmosphereIntegrate.hlsli"

ConstantBuffer<AerialPerspectiveConstants> g_AP : register(b0);

RWTexture2D<float4> u_Color              : register(u0);
Texture2D<float>    t_Depth              : register(t0);
Texture2D           t_TransmittanceLut   : register(t1);
Texture2D           t_MultiScatteringLut : register(t2);
SamplerState        s_LinearClamp        : register(s0);

float LoadConservativeDepth(float2 uv)
{
    uint width;
    uint height;
    t_Depth.GetDimensions(width, height);

    if (all(uint2(width, height) == g_AP.OutputSize))
        return t_Depth.Load(int3(clamp(int2(uv * float2(width, height)), int2(0, 0), int2(width - 1, height - 1)), 0));

    const float2 texelPosition = uv * float2(width, height) - 0.5f;
    const int2 basePixel = int2(floor(texelPosition));
    float selectedDepth = g_AP.ReverseDepth != 0 ? 0.0f : 1.0f;
    bool foundSurface = false;

    [unroll]
    for (int y = 0; y < 2; ++y)
    {
        [unroll]
        for (int x = 0; x < 2; ++x)
        {
            const int2 samplePixel = clamp(basePixel + int2(x, y), int2(0, 0), int2(width - 1, height - 1));
            const float candidate = t_Depth.Load(int3(samplePixel, 0));
            if (candidate <= 1e-7f)
                continue; // Path-tracer invalid/sky sentinel for both projection conventions.

            selectedDepth = foundSurface
                ? (g_AP.ReverseDepth != 0 ? max(selectedDepth, candidate) : min(selectedDepth, candidate))
                : candidate;
            foundSurface = true;
        }
    }

    return foundSurface ? selectedDepth : 0.0f;
}

[numthreads(8, 8, 1)]
void AerialPerspectiveCS(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadID.xy;
    if (any(pixel >= g_AP.OutputSize))
        return;

    const float2 uv = (float2(pixel) + 0.5f) / float2(g_AP.OutputSize);
    const float depth = LoadConservativeDepth(uv);
    if (depth <= 1e-7f || (g_AP.ReverseDepth == 0 && depth >= (1.0f - 1e-7f)))
        return; // The environment cubemap already contains the full atmosphere.

    const float2 clipXY = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 surfaceWorldH = mul(float4(clipXY, depth, 1.0f), g_AP.View.matClipToWorld);
    surfaceWorldH.xyz /= surfaceWorldH.w;

    float3 cameraWorld;
    if (g_AP.View.cameraDirectionOrPosition.w > 0.0f)
    {
        cameraWorld = g_AP.View.cameraDirectionOrPosition.xyz;
    }
    else
    {
        const float nearDepth = g_AP.ReverseDepth != 0 ? 1.0f : 0.0f;
        float4 rayOriginH = mul(float4(clipXY, nearDepth, 1.0f), g_AP.View.matClipToWorld);
        cameraWorld = rayOriginH.xyz / rayOriginH.w;
    }

    const float3 cameraToSurface = surfaceWorldH.xyz - cameraWorld;
    const float distanceWorld = length(cameraToSurface);
    if (distanceWorld <= 1e-6f)
        return;

    const float3 rayWorld = cameraToSurface / distanceWorld;
    const float3 rayAtmosphere = normalize(float3(
        dot(rayWorld, g_AP.AtmosphereBasisXWorld),
        dot(rayWorld, g_AP.AtmosphereBasisYWorld),
        dot(rayWorld, g_AP.AtmosphereBasisZWorld)));
    const float distanceKm = min(distanceWorld * g_AP.WorldToKilometers, g_AP.MaxDistanceKm);
    if (distanceKm <= 0.0f)
        return;

    const float3 atmosphereCamera = float3(
        0.0f, 0.0f, g_AP.Atmosphere.BottomRadius + g_AP.CameraHeightKm);

    SingleScatteringResult atmosphere = IntegrateScatteredLuminance(
        atmosphereCamera,
        rayAtmosphere,
        g_AP.SunDir,
        g_AP.Atmosphere,
        false,
        float(max(g_AP.SampleCount, 1u)),
        false,
        true,
        g_AP.SunIlluminance,
        t_TransmittanceLut,
        s_LinearClamp,
        t_MultiScatteringLut,
        true,
        distanceKm);

    float4 color = u_Color[pixel];
    const float3 inScattering = atmosphere.L * g_AP.RadianceMultiplier;
    color.rgb = color.rgb * atmosphere.Transmittance + inScattering;
    u_Color[pixel] = color;
}

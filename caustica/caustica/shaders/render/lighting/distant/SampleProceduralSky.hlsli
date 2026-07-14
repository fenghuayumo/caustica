#ifndef __SAMPLE_PROCEDURAL_SKY_HLSLI__
#define __SAMPLE_PROCEDURAL_SKY_HLSLI__

// Hillaire 2020 procedural sky sampled from runtime-generated atmosphere LUTs.
#include "SkyAtmosphereCommon.hlsli"

struct ProceduralSkyConstants
{
    AtmosphereParameters Atmosphere;

    float3 SunDir;
    float  CameraHeightKm;

    float3 SunIlluminance;
    float  SunAngularDiameter;

    float  sun_solid_angle;
    float  _pad0;
    float  _pad1;
    float  _pad2;
};

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

struct ProceduralSkyWorkingContext
{
    ProceduralSkyConstants Consts;
    SamplerState           SamplerLinearClamp;
    Texture2D              TransmittanceTexture;
    Texture2D              SkyViewTexture;
};

float3 SampleSkyViewLUT(ProceduralSkyWorkingContext workingContext, float3 WorldPos, float3 WorldDir)
{
    AtmosphereParameters Atmosphere = workingContext.Consts.Atmosphere;
    float viewHeight = length(WorldPos);
    float3 UpVector = WorldPos / viewHeight;
    float viewZenithCosAngle = dot(WorldDir, UpVector);

    float3 sideVector = cross(UpVector, WorldDir);
    float sideLengthSq = dot(sideVector, sideVector);
    sideVector = sideLengthSq > 1e-8f
        ? sideVector * rsqrt(sideLengthSq)
        : float3(0.0f, 1.0f, 0.0f);
    float3 forwardVector = normalize(cross(sideVector, UpVector));
    float2 lightOnPlane = float2(dot(workingContext.Consts.SunDir, forwardVector), dot(workingContext.Consts.SunDir, sideVector));
    float lightPlaneLengthSq = dot(lightOnPlane, lightOnPlane);
    float lightViewCosAngle = lightPlaneLengthSq > 1e-8f
        ? lightOnPlane.x * rsqrt(lightPlaneLengthSq)
        : 1.0f;

    bool IntersectGround = raySphereIntersectNearest(WorldPos, WorldDir, float3(0, 0, 0), Atmosphere.BottomRadius) >= 0.0f;

    float2 uv;
    SkyViewLutParamsToUv(Atmosphere, IntersectGround, viewZenithCosAngle, lightViewCosAngle, viewHeight, uv);
    return workingContext.SkyViewTexture.SampleLevel(workingContext.SamplerLinearClamp, uv, 0).rgb;
}

float3 GetSunDiskLuminance(ProceduralSkyWorkingContext workingContext, float3 WorldPos, float3 WorldDir)
{
    float cosAngle = cos(workingContext.Consts.SunAngularDiameter * 0.5f);
    float softEdge = cos(workingContext.Consts.SunAngularDiameter * 0.55f);
    float sunDisk = smoothstep(softEdge, cosAngle, dot(WorldDir, workingContext.Consts.SunDir));
    if (sunDisk <= 0.0f)
        return 0;

    if (raySphereIntersectNearest(WorldPos, WorldDir, float3(0, 0, 0), workingContext.Consts.Atmosphere.BottomRadius) >= 0.0f)
        return 0;

    float3 transmittance = SampleTransmittanceLUT(
        workingContext.Consts.Atmosphere,
        workingContext.TransmittanceTexture,
        workingContext.SamplerLinearClamp,
        WorldPos,
        workingContext.Consts.SunDir);

    float3 sunLuminance = workingContext.Consts.SunIlluminance * transmittance
        / max(workingContext.Consts.sun_solid_angle, 1e-8f);
    return sunLuminance * sunDisk;
}

float3 ProceduralSky(const float3 viewDirection, ProceduralSkyWorkingContext workingContext)
{
    float3 camera = float3(
        0.0f,
        0.0f,
        workingContext.Consts.Atmosphere.BottomRadius + workingContext.Consts.CameraHeightKm);
    float3 radiance = SampleSkyViewLUT(workingContext, camera, viewDirection);
    return radiance + GetSunDiskLuminance(workingContext, camera, viewDirection);
}

#endif // !cplusplus

#endif // __SAMPLE_PROCEDURAL_SKY_HLSLI__

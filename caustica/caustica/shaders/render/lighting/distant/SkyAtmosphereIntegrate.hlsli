// Hillaire 2020 ray-march integrator (shared by LUT compute shaders)

#ifndef __SKY_ATMOSPHERE_INTEGRATE_HLSLI__
#define __SKY_ATMOSPHERE_INTEGRATE_HLSLI__

#include "SkyAtmosphereCommon.hlsli"

struct SingleScatteringResult
{
    float3 L;
    float3 OpticalDepth;
    float3 Transmittance;
    float3 MultiScatAs1;
};

SingleScatteringResult IntegrateScatteredLuminance(
    float3 WorldPos,
    float3 WorldDir,
    float3 SunDir,
    AtmosphereParameters Atmosphere,
    bool ground,
    float SampleCountIni,
    bool VariableSampleCount,
    bool MieRayPhase,
    float3 sunIlluminance,
    Texture2D TransmittanceLutTexture,
    SamplerState samplerLinearClamp,
    Texture2D MultiScatTexture,
    bool multiScatEnabled,
    float tMaxMax)
{
    SingleScatteringResult result = (SingleScatteringResult)0;

    float3 earthO = float3(0.0f, 0.0f, 0.0f);
    float tBottom = raySphereIntersectNearest(WorldPos, WorldDir, earthO, Atmosphere.BottomRadius);
    float tTop = raySphereIntersectNearest(WorldPos, WorldDir, earthO, Atmosphere.TopRadius);
    float tMax = 0.0f;
    if (tBottom < 0.0f)
    {
        if (tTop < 0.0f)
            return result;
        tMax = tTop;
    }
    else
    {
        if (tTop > 0.0f)
            tMax = min(tTop, tBottom);
        else
            tMax = tBottom;
    }
    tMax = min(tMax, tMaxMax);

    float SampleCount = SampleCountIni;
    float SampleCountFloor = SampleCountIni;
    float tMaxFloor = tMax;
    if (VariableSampleCount)
    {
        SampleCount = lerp(4.0f, 32.0f, saturate(tMax * 0.01f));
        SampleCountFloor = floor(SampleCount);
        tMaxFloor = tMax * SampleCountFloor / SampleCount;
    }

    const float uniformPhase = 1.0f / (4.0f * SKY_ATM_PI);
    float cosTheta = dot(SunDir, WorldDir);
    float MiePhaseValue = hgPhase(Atmosphere.MiePhaseG, -cosTheta);
    float RayleighPhaseValue = RayleighPhase(cosTheta);

    float3 L = 0.0f;
    float3 throughput = 1.0f;
    float3 OpticalDepth = 0.0f;
    float t = 0.0f;
    const float SampleSegmentT = 0.3f;

    for (float s = 0.0f; s < SampleCount; s += 1.0f)
    {
        float dt;
        if (VariableSampleCount)
        {
            float t0 = (s) / SampleCountFloor;
            float t1 = (s + 1.0f) / SampleCountFloor;
            t0 = t0 * t0;
            t1 = t1 * t1;
            t0 = tMaxFloor * t0;
            if (t1 > 1.0f)
                t1 = tMax;
            else
                t1 = tMaxFloor * t1;
            t = t0 + (t1 - t0) * SampleSegmentT;
            dt = t1 - t0;
        }
        else
        {
            const float t0 = tMax * s / SampleCount;
            const float t1 = tMax * (s + 1.0f) / SampleCount;
            dt = t1 - t0;
            t = lerp(t0, t1, SampleSegmentT);
        }

        float3 P = WorldPos + t * WorldDir;
        MediumSampleRGB medium = sampleMediumRGB(P, Atmosphere);
        const float3 SampleOpticalDepth = medium.extinction * dt;
        const float3 SampleTransmittance = exp(-SampleOpticalDepth);
        OpticalDepth += SampleOpticalDepth;

        float pHeight = length(P);
        const float3 UpVector = P / max(pHeight, 1e-6f);
        float SunZenithCosAngle = dot(SunDir, UpVector);
        float3 TransmittanceToSun = SampleTransmittanceLUT(Atmosphere, TransmittanceLutTexture, samplerLinearClamp, P, SunDir);

        float3 PhaseTimesScattering;
        if (MieRayPhase)
            PhaseTimesScattering = medium.scatteringMie * MiePhaseValue + medium.scatteringRay * RayleighPhaseValue;
        else
            PhaseTimesScattering = medium.scattering * uniformPhase;

        float tEarth = raySphereIntersectNearest(P, SunDir, earthO + SKY_ATM_PLANET_RADIUS_OFFSET * UpVector, Atmosphere.BottomRadius);
        float earthShadow = tEarth >= 0.0f ? 0.0f : 1.0f;

        float3 multiScatteredLuminance = 0.0f;
        if (multiScatEnabled)
            multiScatteredLuminance = SampleMultiScatteringLUT(Atmosphere, MultiScatTexture, samplerLinearClamp, P, SunZenithCosAngle);

        float3 S = sunIlluminance * (earthShadow * TransmittanceToSun * PhaseTimesScattering + multiScatteredLuminance * medium.scattering);

        // MultiScatAs1 accumulation (power series input)
        float3 MS = medium.scattering;
        float3 MSint = (MS - MS * SampleTransmittance) / max(medium.extinction, 1e-6f);
        result.MultiScatAs1 += throughput * MSint;

        float3 Sint = (S - S * SampleTransmittance) / max(medium.extinction, 1e-6f);
        L += throughput * Sint;
        throughput *= SampleTransmittance;
    }

    if (ground && tMax == tBottom && tBottom > 0.0f)
    {
        float3 P = WorldPos + tBottom * WorldDir;
        float pHeight = length(P);
        const float3 UpVector = P / max(pHeight, 1e-6f);
        float3 TransmittanceToSun = SampleTransmittanceLUT(Atmosphere, TransmittanceLutTexture, samplerLinearClamp, P, SunDir);
        const float NdotL = saturate(dot(UpVector, SunDir));
        L += sunIlluminance * TransmittanceToSun * throughput * NdotL * Atmosphere.GroundAlbedo / SKY_ATM_PI;
    }

    result.L = L;
    result.OpticalDepth = OpticalDepth;
    result.Transmittance = throughput;
    return result;
}

// Overload for transmittance LUT build (no multi-scatter / no sun illuminance needed for optical depth)
SingleScatteringResult IntegrateOpticalDepthOnly(
    float3 WorldPos,
    float3 WorldDir,
    AtmosphereParameters Atmosphere,
    float SampleCount)
{
    SingleScatteringResult result = (SingleScatteringResult)0;

    float tTop = raySphereIntersectNearest(WorldPos, WorldDir, float3(0, 0, 0), Atmosphere.TopRadius);
    float tBottom = raySphereIntersectNearest(WorldPos, WorldDir, float3(0, 0, 0), Atmosphere.BottomRadius);
    float tMax = 0.0f;
    if (tBottom < 0.0f)
    {
        if (tTop < 0.0f)
            return result;
        tMax = tTop;
    }
    else
        tMax = (tTop > 0.0f) ? min(tTop, tBottom) : tBottom;

    float3 OpticalDepth = 0.0f;
    const float SampleSegmentT = 0.3f;
    for (float s = 0.0f; s < SampleCount; s += 1.0f)
    {
        const float t0 = tMax * s / SampleCount;
        const float t1 = tMax * (s + 1.0f) / SampleCount;
        const float dt = t1 - t0;
        const float t = lerp(t0, t1, SampleSegmentT);
        float3 P = WorldPos + t * WorldDir;
        MediumSampleRGB medium = sampleMediumRGB(P, Atmosphere);
        OpticalDepth += medium.extinction * dt;
    }
    result.OpticalDepth = OpticalDepth;
    result.Transmittance = exp(-OpticalDepth);
    return result;
}

#endif

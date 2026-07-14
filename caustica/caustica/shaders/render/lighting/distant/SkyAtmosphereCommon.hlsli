// Hillaire 2020 — A Scalable and Production Ready Sky and Atmosphere Rendering Technique
// Ported from https://github.com/sebh/UnrealEngineSkyAtmosphere (MIT)

#ifndef __SKY_ATMOSPHERE_COMMON_HLSLI__
#define __SKY_ATMOSPHERE_COMMON_HLSLI__

#ifndef SKY_ATM_PI
#define SKY_ATM_PI 3.14159265358979323846f
#endif

#define SKY_ATM_TRANSMITTANCE_LUT_WIDTH  256.0f
#define SKY_ATM_TRANSMITTANCE_LUT_HEIGHT 64.0f
#define SKY_ATM_MULTISCAT_LUT_RES        32.0f
#define SKY_ATM_SKYVIEW_LUT_WIDTH        192.0f
#define SKY_ATM_SKYVIEW_LUT_HEIGHT       108.0f
#define SKY_ATM_PLANET_RADIUS_OFFSET     0.01f

struct AtmosphereParameters
{
    float  BottomRadius;
    float  TopRadius;
    float  RayleighDensityExpScale;
    float  MieDensityExpScale;

    float3 RayleighScattering;
    float  MiePhaseG;

    float3 MieScattering;
    float  AbsorptionDensity0LayerWidth;

    float3 MieExtinction;
    float  AbsorptionDensity0ConstantTerm;

    float3 MieAbsorption;
    float  AbsorptionDensity0LinearTerm;

    float3 AbsorptionExtinction;
    float  AbsorptionDensity1ConstantTerm;

    float3 GroundAlbedo;
    float  AbsorptionDensity1LinearTerm;

    float  MultiScatteringFactor;
    float  _pad0;
    float  _pad1;
    float  _pad2;
};

// Shared by SkyAtmosphereLUTs.hlsl compute passes (must match C++ writes).
struct SkyAtmosphereLutConstants
{
    AtmosphereParameters Atmosphere;

    float3 SunDir;
    float  CameraHeightKm;

    float3 SunIlluminance;
    float  MultiScatteringLUTRes;

    uint   TransmittanceLutWidth;
    uint   TransmittanceLutHeight;
    uint   SkyViewLutWidth;
    uint   SkyViewLutHeight;
};

struct MediumSampleRGB
{
    float3 scattering;
    float3 absorption;
    float3 extinction;
    float3 scatteringMie;
    float3 scatteringRay;
    float3 albedo;
};

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

float fromUnitToSubUvs(float u, float resolution)
{
    return (u + 0.5f / resolution) * (resolution / (resolution + 1.0f));
}

float fromSubUvsToUnit(float u, float resolution)
{
    return (u - 0.5f / resolution) * (resolution / (resolution - 1.0f));
}

float raySphereIntersectNearest(float3 r0, float3 rd, float3 s0, float sR)
{
    float a = dot(rd, rd);
    float3 s0_r0 = r0 - s0;
    float b = 2.0f * dot(rd, s0_r0);
    float c = dot(s0_r0, s0_r0) - (sR * sR);
    float delta = b * b - 4.0f * a * c;
    if (delta < 0.0f || a == 0.0f)
        return -1.0f;
    float sol0 = (-b - sqrt(delta)) / (2.0f * a);
    float sol1 = (-b + sqrt(delta)) / (2.0f * a);
    if (sol0 < 0.0f && sol1 < 0.0f)
        return -1.0f;
    if (sol0 < 0.0f)
        return max(0.0f, sol1);
    if (sol1 < 0.0f)
        return max(0.0f, sol0);
    return max(0.0f, min(sol0, sol1));
}

float RayleighPhase(float cosTheta)
{
    float factor = 3.0f / (16.0f * SKY_ATM_PI);
    return factor * (1.0f + cosTheta * cosTheta);
}

float hgPhase(float g, float cosTheta)
{
    // Cornette-Shanks as in Hillaire 2020 reference (cosTheta already negated for "in" view dir).
    float k = 3.0f / (8.0f * SKY_ATM_PI) * (1.0f - g * g) / (2.0f + g * g);
    return k * (1.0f + cosTheta * cosTheta) / pow(abs(1.0f + g * g - 2.0f * g * (-cosTheta)), 1.5f);
}

float3 getAlbedo(float3 scattering, float3 extinction)
{
    return scattering / max(0.001f, extinction);
}

MediumSampleRGB sampleMediumRGB(float3 WorldPos, AtmosphereParameters Atmosphere)
{
    const float viewHeight = length(WorldPos) - Atmosphere.BottomRadius;

    const float densityMie = exp(Atmosphere.MieDensityExpScale * viewHeight);
    const float densityRay = exp(Atmosphere.RayleighDensityExpScale * viewHeight);
    const float densityOzo = saturate(viewHeight < Atmosphere.AbsorptionDensity0LayerWidth ?
        Atmosphere.AbsorptionDensity0LinearTerm * viewHeight + Atmosphere.AbsorptionDensity0ConstantTerm :
        Atmosphere.AbsorptionDensity1LinearTerm * viewHeight + Atmosphere.AbsorptionDensity1ConstantTerm);

    MediumSampleRGB s;
    s.scatteringMie = densityMie * Atmosphere.MieScattering;
    float3 extinctionMie = densityMie * Atmosphere.MieExtinction;
    s.scatteringRay = densityRay * Atmosphere.RayleighScattering;
    float3 absorptionOzo = densityOzo * Atmosphere.AbsorptionExtinction;

    s.scattering = s.scatteringMie + s.scatteringRay;
    s.absorption = densityMie * Atmosphere.MieAbsorption + absorptionOzo;
    s.extinction = extinctionMie + s.scatteringRay + absorptionOzo;
    s.albedo = getAlbedo(s.scattering, s.extinction);
    return s;
}

void UvToLutTransmittanceParams(AtmosphereParameters Atmosphere, out float viewHeight, out float viewZenithCosAngle, float2 uv)
{
    float x_mu = uv.x;
    float x_r = uv.y;

    float H = sqrt(max(0.0f, Atmosphere.TopRadius * Atmosphere.TopRadius - Atmosphere.BottomRadius * Atmosphere.BottomRadius));
    float rho = H * x_r;
    viewHeight = sqrt(rho * rho + Atmosphere.BottomRadius * Atmosphere.BottomRadius);

    float d_min = Atmosphere.TopRadius - viewHeight;
    float d_max = rho + H;
    float d = d_min + x_mu * (d_max - d_min);
    viewZenithCosAngle = d == 0.0f ? 1.0f : (H * H - rho * rho - d * d) / (2.0f * viewHeight * d);
    viewZenithCosAngle = clamp(viewZenithCosAngle, -1.0f, 1.0f);
}

void LutTransmittanceParamsToUv(AtmosphereParameters Atmosphere, float viewHeight, float viewZenithCosAngle, out float2 uv)
{
    float H = sqrt(max(0.0f, Atmosphere.TopRadius * Atmosphere.TopRadius - Atmosphere.BottomRadius * Atmosphere.BottomRadius));
    float rho = sqrt(max(0.0f, viewHeight * viewHeight - Atmosphere.BottomRadius * Atmosphere.BottomRadius));

    float discriminant = viewHeight * viewHeight * (viewZenithCosAngle * viewZenithCosAngle - 1.0f) + Atmosphere.TopRadius * Atmosphere.TopRadius;
    float d = max(0.0f, (-viewHeight * viewZenithCosAngle + sqrt(max(0.0f, discriminant))));

    float d_min = Atmosphere.TopRadius - viewHeight;
    float d_max = rho + H;
    float x_mu = (d - d_min) / max(1e-6f, d_max - d_min);
    float x_r = rho / max(1e-6f, H);
    uv = float2(x_mu, x_r);
}

#define NONLINEARSKYVIEWLUT 1

void UvToSkyViewLutParams(AtmosphereParameters Atmosphere, out float viewZenithCosAngle, out float lightViewCosAngle, float viewHeight, float2 uv)
{
    uv = float2(fromSubUvsToUnit(uv.x, SKY_ATM_SKYVIEW_LUT_WIDTH), fromSubUvsToUnit(uv.y, SKY_ATM_SKYVIEW_LUT_HEIGHT));

    float Vhorizon = sqrt(max(0.0f, viewHeight * viewHeight - Atmosphere.BottomRadius * Atmosphere.BottomRadius));
    float CosBeta = Vhorizon / viewHeight;
    float Beta = acos(clamp(CosBeta, -1.0f, 1.0f));
    float ZenithHorizonAngle = SKY_ATM_PI - Beta;

    if (uv.y < 0.5f)
    {
        float coord = 2.0f * uv.y;
        coord = 1.0f - coord;
#if NONLINEARSKYVIEWLUT
        coord *= coord;
#endif
        coord = 1.0f - coord;
        viewZenithCosAngle = cos(ZenithHorizonAngle * coord);
    }
    else
    {
        float coord = uv.y * 2.0f - 1.0f;
#if NONLINEARSKYVIEWLUT
        coord *= coord;
#endif
        viewZenithCosAngle = cos(ZenithHorizonAngle + Beta * coord);
    }

    float coord = uv.x;
    coord *= coord;
    lightViewCosAngle = -(coord * 2.0f - 1.0f);
}

void SkyViewLutParamsToUv(AtmosphereParameters Atmosphere, bool IntersectGround, float viewZenithCosAngle, float lightViewCosAngle, float viewHeight, out float2 uv)
{
    float Vhorizon = sqrt(max(0.0f, viewHeight * viewHeight - Atmosphere.BottomRadius * Atmosphere.BottomRadius));
    float CosBeta = Vhorizon / viewHeight;
    float Beta = acos(clamp(CosBeta, -1.0f, 1.0f));
    float ZenithHorizonAngle = SKY_ATM_PI - Beta;

    if (!IntersectGround)
    {
        float coord = acos(clamp(viewZenithCosAngle, -1.0f, 1.0f)) / ZenithHorizonAngle;
        coord = 1.0f - coord;
#if NONLINEARSKYVIEWLUT
        coord = sqrt(max(coord, 0.0f));
#endif
        coord = 1.0f - coord;
        uv.y = coord * 0.5f;
    }
    else
    {
        float coord = (acos(clamp(viewZenithCosAngle, -1.0f, 1.0f)) - ZenithHorizonAngle) / max(Beta, 1e-6f);
#if NONLINEARSKYVIEWLUT
        coord = sqrt(max(coord, 0.0f));
#endif
        uv.y = coord * 0.5f + 0.5f;
    }

    {
        float coord = -lightViewCosAngle * 0.5f + 0.5f;
        coord = sqrt(max(coord, 0.0f));
        uv.x = coord;
    }

    uv = float2(fromUnitToSubUvs(uv.x, SKY_ATM_SKYVIEW_LUT_WIDTH), fromUnitToSubUvs(uv.y, SKY_ATM_SKYVIEW_LUT_HEIGHT));
}

bool MoveToTopAtmosphere(inout float3 WorldPos, float3 WorldDir, float AtmosphereTopRadius)
{
    float viewHeight = length(WorldPos);
    if (viewHeight > AtmosphereTopRadius)
    {
        float tTop = raySphereIntersectNearest(WorldPos, WorldDir, float3(0, 0, 0), AtmosphereTopRadius);
        if (tTop >= 0.0f)
        {
            float3 UpVector = WorldPos / viewHeight;
            WorldPos = WorldPos + WorldDir * tTop - UpVector * SKY_ATM_PLANET_RADIUS_OFFSET;
        }
        else
            return false;
    }
    return true;
}

float3 SampleTransmittanceLUT(AtmosphereParameters Atmosphere, Texture2D transmittanceLut, SamplerState samp, float3 WorldPos, float3 SunDir)
{
    float viewHeight = length(WorldPos);
    float3 UpVector = WorldPos / viewHeight;
    float SunZenithCosAngle = dot(SunDir, UpVector);
    float2 uv;
    LutTransmittanceParamsToUv(Atmosphere, viewHeight, SunZenithCosAngle, uv);
    return transmittanceLut.SampleLevel(samp, uv, 0).rgb;
}

float3 SampleMultiScatteringLUT(AtmosphereParameters Atmosphere, Texture2D multiScatLut, SamplerState samp, float3 WorldPos, float SunZenithCosAngle)
{
    float2 uv = saturate(float2(SunZenithCosAngle * 0.5f + 0.5f,
        (length(WorldPos) - Atmosphere.BottomRadius) / (Atmosphere.TopRadius - Atmosphere.BottomRadius)));
    uv = float2(fromUnitToSubUvs(uv.x, SKY_ATM_MULTISCAT_LUT_RES), fromUnitToSubUvs(uv.y, SKY_ATM_MULTISCAT_LUT_RES));
    return multiScatLut.SampleLevel(samp, uv, 0).rgb;
}

#endif // !cplusplus

#endif // __SKY_ATMOSPHERE_COMMON_HLSLI__

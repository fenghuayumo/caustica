// Hillaire 2020 LUT compute shaders — Transmittance / Multi-Scattering / Sky-View

#include "SkyAtmosphereIntegrate.hlsli"

ConstantBuffer<SkyAtmosphereLutConstants> g_Lut : register(b0);

RWTexture2D<float4> u_OutLut            : register(u0);
Texture2D           t_TransmittanceLut  : register(t0);
Texture2D           t_MultiScatLut      : register(t1);
SamplerState        s_LinearClamp       : register(s0);

// Dummy black texture path when multi-scat not yet available
Texture2D t_Black : register(t2);

[numthreads(8, 8, 1)]
void TransmittanceLutCS(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= g_Lut.TransmittanceLutWidth || dtid.y >= g_Lut.TransmittanceLutHeight)
        return;

    float2 uv = (float2(dtid.xy) + 0.5f) / float2(g_Lut.TransmittanceLutWidth, g_Lut.TransmittanceLutHeight);
    float viewHeight;
    float viewZenithCosAngle;
    UvToLutTransmittanceParams(g_Lut.Atmosphere, viewHeight, viewZenithCosAngle, uv);

    float3 WorldPos = float3(0.0f, 0.0f, viewHeight);
    float3 WorldDir = float3(0.0f, sqrt(max(0.0f, 1.0f - viewZenithCosAngle * viewZenithCosAngle)), viewZenithCosAngle);

    float3 transmittance = IntegrateOpticalDepthOnly(WorldPos, WorldDir, g_Lut.Atmosphere, 40.0f).Transmittance;
    u_OutLut[dtid.xy] = float4(transmittance, 1.0f);
}

groupshared float3 MultiScatAs1SharedMem[64];
groupshared float3 LSharedMem[64];

[numthreads(1, 1, 64)]
void MultiScattCS(uint3 ThreadId : SV_DispatchThreadID)
{
    float2 pixPos = float2(ThreadId.xy) + 0.5f;
    float2 uv = pixPos / g_Lut.MultiScatteringLUTRes;
    uv = float2(fromSubUvsToUnit(uv.x, g_Lut.MultiScatteringLUTRes), fromSubUvsToUnit(uv.y, g_Lut.MultiScatteringLUTRes));

    AtmosphereParameters Atmosphere = g_Lut.Atmosphere;

    float cosSunZenithAngle = uv.x * 2.0f - 1.0f;
    float3 sunDir = float3(0.0f, sqrt(saturate(1.0f - cosSunZenithAngle * cosSunZenithAngle)), cosSunZenithAngle);
    float viewHeight = Atmosphere.BottomRadius + saturate(uv.y + SKY_ATM_PLANET_RADIUS_OFFSET) * (Atmosphere.TopRadius - Atmosphere.BottomRadius - SKY_ATM_PLANET_RADIUS_OFFSET);

    float3 WorldPos = float3(0.0f, 0.0f, viewHeight);

    const float SphereSolidAngle = 4.0f * SKY_ATM_PI;
    const float IsotropicPhase = 1.0f / SphereSolidAngle;

#define SQRTSAMPLECOUNT 8
    const float sqrtSample = float(SQRTSAMPLECOUNT);
    float i = 0.5f + float(ThreadId.z / SQRTSAMPLECOUNT);
    float j = 0.5f + float(ThreadId.z - (ThreadId.z / SQRTSAMPLECOUNT) * SQRTSAMPLECOUNT);

    float randA = i / sqrtSample;
    float randB = j / sqrtSample;
    float theta = 2.0f * SKY_ATM_PI * randA;
    float phi = acos(1.0f - 2.0f * randB);
    float3 WorldDir = float3(cos(theta) * sin(phi), sin(theta) * sin(phi), cos(phi));

    // Multi-scat build uses illuminance=1 and no recursive multi-scat (seed table)
    SingleScatteringResult result = IntegrateScatteredLuminance(
        WorldPos, WorldDir, sunDir, Atmosphere,
        true, 20.0f, false, false,
        float3(1, 1, 1),
        t_TransmittanceLut, s_LinearClamp,
        t_Black, false,
        9000000.0f);

    MultiScatAs1SharedMem[ThreadId.z] = result.MultiScatAs1 * SphereSolidAngle / (sqrtSample * sqrtSample);
    LSharedMem[ThreadId.z] = result.L * SphereSolidAngle / (sqrtSample * sqrtSample);
#undef SQRTSAMPLECOUNT

    GroupMemoryBarrierWithGroupSync();

    if (ThreadId.z < 32) { MultiScatAs1SharedMem[ThreadId.z] += MultiScatAs1SharedMem[ThreadId.z + 32]; LSharedMem[ThreadId.z] += LSharedMem[ThreadId.z + 32]; }
    GroupMemoryBarrierWithGroupSync();
    if (ThreadId.z < 16) { MultiScatAs1SharedMem[ThreadId.z] += MultiScatAs1SharedMem[ThreadId.z + 16]; LSharedMem[ThreadId.z] += LSharedMem[ThreadId.z + 16]; }
    GroupMemoryBarrierWithGroupSync();
    if (ThreadId.z < 8)  { MultiScatAs1SharedMem[ThreadId.z] += MultiScatAs1SharedMem[ThreadId.z + 8];  LSharedMem[ThreadId.z] += LSharedMem[ThreadId.z + 8]; }
    GroupMemoryBarrierWithGroupSync();
    if (ThreadId.z < 4)  { MultiScatAs1SharedMem[ThreadId.z] += MultiScatAs1SharedMem[ThreadId.z + 4];  LSharedMem[ThreadId.z] += LSharedMem[ThreadId.z + 4]; }
    GroupMemoryBarrierWithGroupSync();
    if (ThreadId.z < 2)  { MultiScatAs1SharedMem[ThreadId.z] += MultiScatAs1SharedMem[ThreadId.z + 2];  LSharedMem[ThreadId.z] += LSharedMem[ThreadId.z + 2]; }
    GroupMemoryBarrierWithGroupSync();
    if (ThreadId.z < 1)  { MultiScatAs1SharedMem[ThreadId.z] += MultiScatAs1SharedMem[ThreadId.z + 1];  LSharedMem[ThreadId.z] += LSharedMem[ThreadId.z + 1]; }
    GroupMemoryBarrierWithGroupSync();
    if (ThreadId.z > 0)
        return;

    float3 MultiScatAs1 = MultiScatAs1SharedMem[0] * IsotropicPhase;
    float3 InScatteredLuminance = LSharedMem[0] * IsotropicPhase;
    const float3 r = MultiScatAs1;
    const float3 SumOfAll = 1.0f / max(1.0f - r, 1e-4f);
    float3 L = InScatteredLuminance * SumOfAll;

    u_OutLut[ThreadId.xy] = float4(Atmosphere.MultiScatteringFactor * L, 1.0f);
}

[numthreads(8, 8, 1)]
void SkyViewLutCS(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= g_Lut.SkyViewLutWidth || dtid.y >= g_Lut.SkyViewLutHeight)
        return;

    AtmosphereParameters Atmosphere = g_Lut.Atmosphere;
    float2 uv = (float2(dtid.xy) + 0.5f) / float2(g_Lut.SkyViewLutWidth, g_Lut.SkyViewLutHeight);

    float viewHeight = Atmosphere.BottomRadius + g_Lut.CameraHeightKm;
    float viewZenithCosAngle;
    float lightViewCosAngle;
    UvToSkyViewLutParams(Atmosphere, viewZenithCosAngle, lightViewCosAngle, viewHeight, uv);

    float3 UpVector = float3(0, 0, 1);
    float sunZenithCosAngle = dot(UpVector, g_Lut.SunDir);
    float3 SunDir = normalize(float3(sqrt(max(0.0f, 1.0f - sunZenithCosAngle * sunZenithCosAngle)), 0.0f, sunZenithCosAngle));

    float3 WorldPos = float3(0.0f, 0.0f, viewHeight);
    float viewZenithSinAngle = sqrt(max(0.0f, 1.0f - viewZenithCosAngle * viewZenithCosAngle));
    float3 WorldDir = float3(
        viewZenithSinAngle * lightViewCosAngle,
        viewZenithSinAngle * sqrt(max(0.0f, 1.0f - lightViewCosAngle * lightViewCosAngle)),
        viewZenithCosAngle);

    if (!MoveToTopAtmosphere(WorldPos, WorldDir, Atmosphere.TopRadius))
    {
        u_OutLut[dtid.xy] = float4(0, 0, 0, 1);
        return;
    }

    SingleScatteringResult ss = IntegrateScatteredLuminance(
        WorldPos, WorldDir, SunDir, Atmosphere,
        false, 30.0f, true, true,
        g_Lut.SunIlluminance,
        t_TransmittanceLut, s_LinearClamp,
        t_MultiScatLut, true,
        9000000.0f);

    u_OutLut[dtid.xy] = float4(ss.L, 1.0f);
}

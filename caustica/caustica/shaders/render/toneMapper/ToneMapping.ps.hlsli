 #ifndef __TONE_MAPPING_PS_HLSLI__ // using instead of "#pragma once" due to https://github.com/microsoft/DirectXShaderCompiler/issues/3943
 #define __TONE_MAPPING_PS_HLSLI__

#include "ToneMapping_cb.h"

SamplerState gLuminanceTexSampler : register(s0);
SamplerState gColorSampler : register(s1);

Texture2D gColorTex : register(t0);
Texture2D gLuminanceTex : register(t1);

//static const uint kOperator = _TONE_MAPPER_OPERATOR;
static const float kExposureKey = TONEMAPPING_EXPOSURE_KEY;
static const float kLuminanceLod = 16.0; // Lookup highest mip level to get average luminance

cbuffer PerImageCB : register(b0)
{
    ToneMappingConstants gParams;
};

float calcLuminance(float3 color)
{
    return dot(color, float3(0.299, 0.587, 0.114));
}

// Linear
float3 toneMapLinear(float3 color)
{
    return color;
}

// Reinhard 2002, eq. 3: Ld = L / (1 + L), applied to luminance and
// reapplied to RGB to keep chromaticity. Equivalent to color / (1 + L)
// and defined at L = 0 (the previous L_out / L form was 0/0 at black).
float3 toneMapReinhard(float3 color)
{
    float luminance = calcLuminance(color);
    return color / (1.0 + luminance);
}

// Reinhard 2002, eq. 4: Ld = L * (1 + L / Lwhite^2) / (1 + L).
// The previous implementation multiplied by (1 + L) instead of dividing,
// which expanded highlights instead of compressing them.
float3 toneMapReinhardModified(float3 color)
{
    float luminance = calcLuminance(color);
    float white2 = max(gParams.whiteMaxLuminance * gParams.whiteMaxLuminance, 1e-6);
    float scale = (1.0 + luminance / white2) / (1.0 + luminance);
    return color * scale;
}

// John Hable's ALU approximation of Jim Heji's operator
// http://filmicgames.com/archives/75
float3 toneMapHejiHableAlu(float3 color)
{
    color = max(float(0).rrr, color - 0.004);
    color = (color*(6.2 * color + 0.5)) / (color * (6.2 * color + 1.7) + 0.06);

    // Result includes sRGB conversion
    return pow(color, float3(2.2, 2.2, 2.2));
}

// John Hable's Uncharted 2 filmic tone map
// http://filmicgames.com/archives/75
float3 applyUc2Curve(float3 color)
{
    float A = 0.22; // Shoulder Strength
    float B = 0.3;  // Linear Strength
    float C = 0.1;  // Linear Angle
    float D = 0.2;  // Toe Strength
    float E = 0.01; // Toe Numerator
    float F = 0.3;  // Toe Denominator

    color = ((color * (A*color+C*B)+D*E)/(color*(A*color+B)+D*F))-(E/F);
    return color;
}

float3 toneMapHableUc2(float3 color)
{
    float exposureBias = 2.0f;
    color = applyUc2Curve(exposureBias * color);
    float whiteScale = 1 / applyUc2Curve(float3(gParams.whiteScale, gParams.whiteScale, gParams.whiteScale)).x;
    color = color * whiteScale;

    return color;
}

float3 toneMapAces(float3 color)
{
    // Cancel out the pre-exposure mentioned in
    // https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
    color *= 0.6;

    float A = 2.51;
    float B = 0.03;
    float C = 2.43;
    float D = 0.59;
    float E = 0.14;

    color = saturate((color*(A*color+B))/(color*(C*color+D)+E));
    return color;
}

// Khronos PBR Neutral. Linear Rec.709 in/out, display range ~[0, 1].
// Midtones stay near-linear; only highlights and oversaturation are compressed.
// https://github.com/KhronosGroup/ToneMapping/blob/main/PBR_Neutral/pbrNeutral.glsl
float3 toneMapPbrNeutral(float3 color)
{
    color = max(color, 0.0);

    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
        return color;

    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return lerp(color, newPeak.xxx, g);
}

// Identity below `t`, C1-continuous Reinhard shoulder above it.
// Per-channel so SDR values are bit-stable (photo matching); highlights
// compress toward 1. Inverse for y > t: x = t + s * e / (s - e), e = y - t.
float identitySoftShoulderChannel(float x)
{
    const float t = 0.8;
    const float s = 1.0 - t;
    float excess = max(x - t, 0.0);
    return min(x, t) + s * excess / (excess + s);
}

float3 toneMapIdentitySoftShoulder(float3 color)
{
    return float3(
        identitySoftShoulderChannel(color.r),
        identitySoftShoulderChannel(color.g),
        identitySoftShoulderChannel(color.b));
}

// 6th-order sigmoid used by Filament / three.js to approximate AgX contrast.
// Mean error^2: 3.6705141e-06
float3 agxDefaultContrastApprox(float3 x)
{
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return + 15.5 * x4 * x2
           - 40.14 * x4 * x
           + 31.96 * x4
           - 6.868 * x2 * x
           + 0.4298 * x2
           + 0.1191 * x
           - 0.00232;
}

// AgX (Filament / three.js realtime approximation of Blender AgX).
// Linear Rec.709 in/out. Rec.2020 inset, log2 encode, sigmoid, outset,
// then 2.2 EOTF so the later sRGB encode matches other operators here.
// https://github.com/mrdoob/three.js/blob/dev/src/renderers/shaders/ShaderChunk/tonemapping_pars_fragment.glsl.js
float3 toneMapAgX(float3 color)
{
    const float3x3 linearSrgbToLinearRec2020 = float3x3(
         0.6274,  0.3293,  0.0433,
         0.0691,  0.9195,  0.0113,
         0.0164,  0.0880,  0.8956);

    const float3x3 linearRec2020ToLinearSrgb = float3x3(
         1.6605, -0.5876, -0.0728,
        -0.1246,  1.1329, -0.0083,
        -0.0182, -0.1006,  1.1187);

    const float3x3 agxInset = float3x3(
         0.856627153315983,  0.0951212405381588, 0.0482516061458583,
         0.137318972929847,  0.761241990602591,  0.101439036467562,
         0.11189821299995,   0.0767994186031903, 0.811302368396859);

    const float3x3 agxOutset = float3x3(
         1.1271005818144368,  -0.11060664309660323, -0.016493938717834573,
        -0.1413297634984383,   1.157823702216272,   -0.016493938717834257,
        -0.14132976349843826, -0.11060664309660294,  1.2519364065950405);

    const float agxMinEv = -12.47393;
    const float agxMaxEv = 4.026069;

    color = mul(linearSrgbToLinearRec2020, color);
    color = mul(agxInset, color);

    color = max(color, 1e-10);
    color = (log2(color) - agxMinEv) / (agxMaxEv - agxMinEv);
    color = saturate(color);

    color = agxDefaultContrastApprox(color);
    color = mul(agxOutset, color);

    color = pow(max(color, 0.0), 2.2);
    color = mul(linearRec2020ToLinearSrgb, color);
    return saturate(color);
}

float3 toneMap(float3 color)
{
    switch ((ToneMapperOperator)gParams.toneMapOperator)
    {
        case ToneMapperOperator::Linear:
            return toneMapLinear(color);
        case ToneMapperOperator::Reinhard:
            return toneMapReinhard(color);
        case ToneMapperOperator::ReinhardModified:
            return toneMapReinhardModified(color);
        case ToneMapperOperator::HejiHableAlu:
            return toneMapHejiHableAlu(color);
        case ToneMapperOperator::HableUc2:
            return toneMapHableUc2(color);
        case ToneMapperOperator::Aces:
            return toneMapAces(color);
        case ToneMapperOperator::PbrNeutral:
            return toneMapPbrNeutral(color);
        case ToneMapperOperator::IdentitySoftShoulder:
            return toneMapIdentitySoftShoulder(color);
        case ToneMapperOperator::AgX:
            return toneMapAgX(color);
        default:
            return color;
    }
}

//Renamed main function 
//float4 main(float2 texC : TEXCOORD) : SV_TARGET0
float4 applyToneMapping(float2 texC)
{
    float4 color = gColorTex.Sample(gColorSampler, texC);
    float3 finalColor = color.rgb;
/*
#ifdef _TONE_MAPPER_AUTO_EXPOSURE
    // apply auto exposure
    float avgLuminance = exp2(gLuminanceTex.SampleLevel(gLuminanceTexSampler, texC, kLuminanceLod).r);
    float pixelLuminance = calcLuminance(finalColor);
    finalColor *= (kExposureKey / avgLuminance);
#endif
*/
    if(gParams.autoExposure)
    {
        // apply auto exposure

#ifndef TONEMAPPING_AUTOEXPOSURE_CPU
#error this must be defined
#elif TONEMAPPING_AUTOEXPOSURE_CPU == 1
        float avgLuminance = gParams.avgLuminance;
#else
        float avgLuminance = exp2(gLuminanceTex.SampleLevel(gLuminanceTexSampler, texC, kLuminanceLod).r);
#endif
        float pixelLuminance = calcLuminance(finalColor);

        finalColor *= clamp( (kExposureKey / avgLuminance), gParams.autoExposureLumValueMin, gParams.autoExposureLumValueMax );
    }

    if (gParams.enabled)
    {
        // apply color grading
        finalColor = mul(finalColor, (float3x3) gParams.colorTransform);

        // apply tone mapping
        finalColor = toneMap(finalColor);

        if (gParams.clamped)
            finalColor = saturate(finalColor);
    }

    return float4(finalColor, color.a);
}

#endif //__TONE_MAPPING_PS_HLSLI__

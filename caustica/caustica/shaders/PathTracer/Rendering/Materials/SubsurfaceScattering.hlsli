// Adapted for caustica from the NVIDIA RTX Character Rendering SDK.
// Upstream algorithms: Chiang et al. 2016 hair BCSDF (https://benedikt-bitterli.me/pchfm/,
// https://www.pbrt.org/hair.pdf) and the Burley normalized diffusion profile.
//
// Copyright (c) 2024-2025, NVIDIA CORPORATION. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef __SUBSURFACE_SCATTERING_HLSLI__
#define __SUBSURFACE_SCATTERING_HLSLI__

#include "ScatteringCommon.hlsli"
#include "SubsurfaceMaterial.hlsli"

float3 BurleyScalingFactor(float3 albedo)
{
#ifdef USE_DIFFUSE_MEAN_FREE_PATH
    const float3 a33 = albedo - 0.33f;
    const float3 a332 = a33 * a33;
    return 3.5f + 100.0f * a332 * a332;
#else
    const float3 absa = abs(albedo - 0.8f);
    return 1.85f - albedo + 7.0f * absa * absa * absa;
#endif
}

float4 SampleBurleyProfileMIS(
    in float rand, in const float3 mfp, in const float3 diffuseAlbedo,
    in const float3 ssAlbedo, in const bool enableTransmission)
{
    const float3 albedoNormalized = diffuseAlbedo
        / max(diffuseAlbedo.r + diffuseAlbedo.g + diffuseAlbedo.b, 1e-7f).xxx;
    const float2 channelCdf = float2(albedoNormalized.x,
        albedoNormalized.x + albedoNormalized.y);
    uint channel = 0;
    if (rand < channelCdf.x)
    {
        rand /= max(channelCdf.x, 1e-7f);
    }
    else if (rand < channelCdf.y)
    {
        rand = (rand - channelCdf.x) / max(albedoNormalized.y, 1e-7f);
        channel = 1;
    }
    else
    {
        rand = (rand - channelCdf.y) / max(albedoNormalized.z, 1e-7f);
        channel = 2;
    }

    rand = clamp(rand, 1e-7f, 1.0f - 1e-7f);
    const float3 s = BurleyScalingFactor(diffuseAlbedo);
    const float3 d = max(mfp * s, 1e-7f);

    float r;
    if (rand < 0.25f)
    {
        rand *= 4.0f;
        r = -log(max(rand, 1e-7f)) / d[channel];
    }
    else
    {
        rand = (rand - 0.25f) / 0.75f;
        r = -3.0f * log(max(rand, 1e-7f)) / d[channel];
    }

    const float3 pdf3 = 0.25f * d * (exp(-r * d) + exp(-r * d / 3.0f));
    const float3 pdfSS = enableTransmission
        ? 0.266f * ssAlbedo * (exp(-5.434f * mfp * r) + exp(-1.811f * mfp * r)) * mfp
        : 0.0f;
    return float4((diffuseAlbedo * pdf3 - pdfSS)
        / max(dot(albedoNormalized, pdf3), 1e-7f).xxx, r);
}

void EvalBurleyDiffusionProfile(
    in const SubsurfaceMaterialData material,
    in const SubsurfaceInteraction interaction,
    in const float maxSampleRadius, in const bool enableTransmission,
    in const float2 rand2, out SubsurfaceSample sample)
{
    const SubsurfaceMaterialCoefficients coefficients =
        ComputeSubsurfaceMaterialCoefficients(material);
    const float4 profile = SampleBurleyProfileMIS(rand2.x,
        coefficients.sigma_t, coefficients.albedo,
        coefficients.ssAlbedo, enableTransmission);
    const float r = profile.w;
    const float l = sqrt(max(maxSampleRadius * maxSampleRadius - r * r, 1e-7f));
    sample.samplePosition = CalculateDiskSamplePosition(rand2.y, r,
        interaction.centerPosition, interaction.tangent, interaction.biTangent)
        + interaction.normal * l;
    sample.bssrdfWeight = profile.xyz;
}

float3 EvalBssrdf(in const SubsurfaceSample sample,
    in const float3 incidentRadiance, in const float NoL)
{
    return SCATTER_ONE_OVER_PI * sample.bssrdfWeight
        * incidentRadiance * saturate(NoL).xxx;
}

#endif

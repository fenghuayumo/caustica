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

#ifndef __SUBSURFACE_MATERIAL_HLSLI__
#define __SUBSURFACE_MATERIAL_HLSLI__

#include "ScatteringMath.hlsli"

#define MAX_SSS_SAMPLE_COUNT 4
#define SSS_METERS_UNIT 0.01f
#define SSS_MIN_ALBEDO 0.01f

struct SubsurfaceMaterialData
{
    float3 transmissionColor;
    float  g;
    float3 scatteringColor;
    float  scale;
};

struct SubsurfaceInteraction
{
    float3 centerPosition;
    float3 normal;
    float3 tangent;
    float3 biTangent;
};

struct SubsurfaceSample
{
    float3 samplePosition;
    float3 bssrdfWeight;
};

struct VolumeCoefficients
{
    float3 scattering;
    float3 absorption;
};

struct SubsurfaceMaterialCoefficients
{
    float3 sigma_s;
    float3 sigma_t;
    float3 albedo;
    float3 ssAlbedo;
};

SubsurfaceMaterialData CreateDefaultSubsurfaceMaterialData()
{
    SubsurfaceMaterialData data;
    data.transmissionColor = 0.0f;
    data.scatteringColor = 0.0f;
    data.g = 0.0f;
    data.scale = 0.0f;
    return data;
}

SubsurfaceInteraction CreateSubsurfaceInteraction(
    const float3 centerPosition, const float3 normal,
    const float3 tangent, const float3 biTangent)
{
    SubsurfaceInteraction interaction;
    interaction.centerPosition = centerPosition;
    interaction.normal = normal;
    interaction.tangent = tangent;
    interaction.biTangent = biTangent;
    return interaction;
}

float3 ComputeTransmissionAlbedo(in const float3 transmissionColor)
{
    return 4.09712f + 4.20863f * transmissionColor
        - ScatterSqrt0(9.59217f + 41.6808f * transmissionColor
        + 17.7126f * transmissionColor * transmissionColor);
}

VolumeCoefficients ComputeSubsurfaceVolumeCoefficients(
    in const SubsurfaceMaterialData sssData)
{
    const float3 s = ComputeTransmissionAlbedo(sssData.transmissionColor);
    const float3 alpha = (SCATTER_WHITE_COLOR - s * s)
        / max(SCATTER_WHITE_COLOR - sssData.g * (s * s), 1e-7f);
    const float scale = SSS_METERS_UNIT * sssData.scale;
    const float3 scatteringRadius = max(scale.xxx * sssData.scatteringColor, 1e-7f);

    VolumeCoefficients coefficients;
    coefficients.scattering = alpha / scatteringRadius;
    coefficients.absorption = SCATTER_WHITE_COLOR / scatteringRadius - coefficients.scattering;
    return coefficients;
}

SubsurfaceMaterialCoefficients ComputeSubsurfaceMaterialCoefficients(
    in const SubsurfaceMaterialData sssData)
{
    const VolumeCoefficients volume = ComputeSubsurfaceVolumeCoefficients(sssData);
    const float3 sigmaA = volume.absorption;
    const float3 sigmaS = volume.scattering;
    const float3 sigmaT = max(sigmaA + sigmaS, 1e-7f);
    const float3 mfp = 1.0f / sigmaT;
    const float3 s = ScatterSqrt0(sigmaA * mfp);

    SubsurfaceMaterialCoefficients coefficients;
    coefficients.sigma_s = sigmaS;
    coefficients.sigma_t = sigmaT;
    coefficients.albedo = 0.88f * (1.0f - s) / (1.0f + 1.5535f * s);
    coefficients.ssAlbedo = max(SSS_MIN_ALBEDO, sigmaS / sigmaT);
    return coefficients;
}

#endif

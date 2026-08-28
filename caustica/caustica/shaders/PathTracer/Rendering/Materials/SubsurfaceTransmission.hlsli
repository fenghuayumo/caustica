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

#ifndef __SUBSURFACE_TRANSMISSION_HLSLI__
#define __SUBSURFACE_TRANSMISSION_HLSLI__

#include "ScatteringCommon.hlsli"
#include "SubsurfaceMaterial.hlsli"

float3 CalculateRefractionRay(
    in const SubsurfaceInteraction interaction, in const float2 rand2)
{
    float samplePdf = 0.0f;
    const float3 sampleDirectionLocal = ScatterSampleHemisphere(rand2, samplePdf);
    const float3x3 tangentBasis = float3x3(
        interaction.tangent, -interaction.biTangent, -interaction.normal);
    return mul(transpose(tangentBasis), sampleDirectionLocal);
}

float3 EvaluateBoundaryTerm(
    in const float3 normal, in const float3 vectorToLight,
    in const float3 refractedRayDirection, in const float3 backfaceNormal,
    in const float thickness,
    in const SubsurfaceMaterialCoefficients coefficients)
{
    const float3 boundaryBsdf = EvalLambertianBRDF(
        backfaceNormal, vectorToLight, coefficients.albedo);
    const float3 frontBsdf = EvalLambertianBRDF(
        -normal, refractedRayDirection, coefficients.albedo);
    const float3 attenuation = EvalBeerLambertAttenuation(
        coefficients.sigma_t, thickness);
    return boundaryBsdf * attenuation * frontBsdf;
}

float3 EvaluateSingleScattering(
    in const float3 vectorToLight, in const float3 scatteringBoundaryNormal,
    in const float totalScatteringDistance,
    in const SubsurfaceMaterialCoefficients coefficients)
{
    const float3 boundaryBsdf = EvalLambertianBRDF(
        scatteringBoundaryNormal, vectorToLight, coefficients.albedo);
    const float3 attenuation = EvalBeerLambertAttenuation(
        coefficients.sigma_t, totalScatteringDistance);
    return coefficients.sigma_s * boundaryBsdf * attenuation;
}

#endif

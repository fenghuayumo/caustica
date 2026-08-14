/*
* Copyright (c) 2024-2025, NVIDIA CORPORATION. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*/

#ifndef _RTXCR_SUBSURFACETRANSMISSION_HLSLI_
#define _RTXCR_SUBSURFACETRANSMISSION_HLSLI_

#include "utils/RtxcrBsdf.hlsli"
#include "SubsurfaceMaterial.hlsli"

float3 RTXCR_CalculateRefractionRay(
    in const RTXCR_SubsurfaceInteraction interaction, in const float2 rand2)
{
    float samplePdf = 0.0f;
    const float3 sampleDirectionLocal = RTXCR_SampleHemisphere(rand2, samplePdf);
    const float3x3 tangentBasis = float3x3(
        interaction.tangent, -interaction.biTangent, -interaction.normal);
    return mul(transpose(tangentBasis), sampleDirectionLocal);
}

float3 RTXCR_EvaluateBoundaryTerm(
    in const float3 normal, in const float3 vectorToLight,
    in const float3 refractedRayDirection, in const float3 backfaceNormal,
    in const float thickness,
    in const RTXCR_SubsurfaceMaterialCoefficients coefficients)
{
    const float3 boundaryBsdf = RTXCR_EvalLambertianBRDF(
        backfaceNormal, vectorToLight, coefficients.albedo);
    const float3 frontBsdf = RTXCR_EvalLambertianBRDF(
        -normal, refractedRayDirection, coefficients.albedo);
    const float3 attenuation = RTXCR_EvalBeerLambertAttenuation(
        coefficients.sigma_t, thickness);
    return boundaryBsdf * attenuation * frontBsdf;
}

float3 RTXCR_EvaluateSingleScattering(
    in const float3 vectorToLight, in const float3 scatteringBoundaryNormal,
    in const float totalScatteringDistance,
    in const RTXCR_SubsurfaceMaterialCoefficients coefficients)
{
    const float3 boundaryBsdf = RTXCR_EvalLambertianBRDF(
        scatteringBoundaryNormal, vectorToLight, coefficients.albedo);
    const float3 attenuation = RTXCR_EvalBeerLambertAttenuation(
        coefficients.sigma_t, totalScatteringDistance);
    return coefficients.sigma_s * boundaryBsdf * attenuation;
}

#endif

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

#ifndef __HAIR_BSDF_HELPERS_HLSLI__
#define __HAIR_BSDF_HELPERS_HLSLI__

// Longitudinal scattering function Mp.
float HairMp(const float cosThetaI, const float cosThetaO, const float sinThetaI, const float sinThetaO, const float v)
{
    const float a = cosThetaI * cosThetaO / v;
    const float b = sinThetaI * sinThetaO / v;
    const float mp = (v <= 0.1f) ? exp(LogBesselI0(a) - b - 1.0f / v + 0.6931f + log(0.5f / v)) : (exp(-b) * BesselI0(a)) / (sinh(1.0f / v) * 2.0f * v);
    return mp;
}

// Attenuation function Ap.
void HairAp(const HairMaterialInteraction hairMaterialInteraction, const float cosThetaI, const float3 T, out float3 ap[Hair_Max_Scattering_Events + 1])
{
    const float cosGammaI = ScatterSqrt01(1.0f - hairMaterialInteraction.h * hairMaterialInteraction.h);
    const float cosTheta = cosThetaI * cosGammaI;
    const float3 f = hairMaterialInteraction.fresnelApproximation ?
        ScatterFresnelSchlick(CalculateBaseReflectivity(1.0f, hairMaterialInteraction.ior), cosTheta).rrr :
        DielectricFresnel(hairMaterialInteraction.ior, cosTheta).rrr;

    ap[0] = f;
    ap[1] = T * (float3(1.0f, 1.0f, 1.0f) - f) * (float3(1.0f, 1.0f, 1.0f) - f);
    [unroll]
    for (uint p = 2; p < Hair_Max_Scattering_Events; ++p)
    {
        ap[p] = ap[p - 1] * T * f;
    }

    // Compute attenuation term accounting for remaining orders of scattering.
    ap[Hair_Max_Scattering_Events] = ap[Hair_Max_Scattering_Events - 1] * T * f / (float3(1.0f, 1.0f, 1.0f) - T * f);
}

// Azimuthal scattering function Np.
float HairNp(const float phi, const int p, const float s, const float gammaI, const float gammaT)
{
    float dphi = phi - PhiFunction(p, gammaI, gammaT);

    // Remap dphi to [-pi, pi].
    dphi = fmod(dphi, SCATTER_TWO_PI);
    if (dphi > SCATTER_PI)
    {
        dphi -= SCATTER_TWO_PI;
    }
    if (dphi < -SCATTER_PI)
    {
        dphi += SCATTER_TWO_PI;
    }

    return HairTrimmedLogistic(dphi, s, -SCATTER_PI, SCATTER_PI);
}

// Compute a discrete pdf for sampling Ap (Lobe selection)
void ComputeApPdf(const HairMaterialInteraction hairMaterialInteraction, const float cosThetaO, out float apPdf[Hair_Max_Scattering_Events + 1])
{
    const float sinThetaO = ScatterSqrt01(1.0f - cosThetaO * cosThetaO);

    // Compute refracted ray.
    const float sinThetaT = sinThetaO / hairMaterialInteraction.ior;
    const float cosThetaT = ScatterSqrt01(1.0f - sinThetaT * sinThetaT);

    const float etap = ScatterSqrt0(hairMaterialInteraction.ior * hairMaterialInteraction.ior - sinThetaO * sinThetaO) / cosThetaO;
    const float sinGammaT = hairMaterialInteraction.h / etap;
    const float cosGammaT = ScatterSqrt01(1.0f - sinGammaT * sinGammaT);

    // Compute the transmittance T of a single path through the cylinder.
    const float tmp = -2.0f * cosGammaT / cosThetaT;
    const float3 T = exp(hairMaterialInteraction.absorptionCoefficient * tmp);

    float3 ap[Hair_Max_Scattering_Events + 1];
    HairAp(hairMaterialInteraction, cosThetaO, T, ap);

    // Compute apPdf from individal ap terms.
    float sumY = 0.0f;
    [unroll]
    for (uint p = 0; p < Hair_Max_Scattering_Events; ++p)
    {
        apPdf[p] = ScatterLuminance(ap[p]);
        sumY += apPdf[p];
    }

    const float invSumY = 1.0f / sumY;
    [unroll]
    for (uint p2 = 0; p2 < Hair_Max_Scattering_Events; ++p2)
    {
        apPdf[p2] *= invSumY;
    }

    apPdf[Hair_Max_Scattering_Events] = 0.0f;
}

void ApSeparateChiang(const HairMaterialSeparateChiangInteraction hairMaterialSeparateChiangInteraction, const float cosThetaI, const float3 T, out float3 ap[Hair_Max_Scattering_Events + 1])
{
    const float cosGammaI = ScatterSqrt01(1.0f - hairMaterialSeparateChiangInteraction.h * hairMaterialSeparateChiangInteraction.h);
    const float cosTheta = cosThetaI * cosGammaI;
    const float3 f = hairMaterialSeparateChiangInteraction.fresnelApproximation ?
        ScatterFresnelSchlick(CalculateBaseReflectivity(1.0f, hairMaterialSeparateChiangInteraction.ior), cosTheta).rrr :
        DielectricFresnel(hairMaterialSeparateChiangInteraction.ior, cosTheta).rrr;

    ap[0] = f;
    ap[1] = T * (float3(1.0f, 1.0f, 1.0f) - f) * (float3(1.0f, 1.0f, 1.0f) - f);
    [unroll]
    for (uint p = 2; p < Hair_Max_Scattering_Events; ++p)
    {
        ap[p] = ap[p - 1] * T * f;
    }

    // Compute attenuation term accounting for remaining orders of scattering.
    ap[Hair_Max_Scattering_Events] = ap[Hair_Max_Scattering_Events - 1] * T * f / (float3(1.0f, 1.0f, 1.0f) - T * f);
}

void ComputeSeparateChiangApPdf(const HairMaterialSeparateChiangInteraction hairMaterialSeparateChiangInteraction, const float cosThetaO, out float apPdf[Hair_Max_Scattering_Events + 1])
{
    const float sinThetaO = ScatterSqrt01(1.0f - cosThetaO * cosThetaO);

    // Compute refracted ray.
    const float sinThetaT = sinThetaO / hairMaterialSeparateChiangInteraction.ior;
    const float cosThetaT = ScatterSqrt01(1.0f - sinThetaT * sinThetaT);

    const float etap = ScatterSqrt0(hairMaterialSeparateChiangInteraction.ior * hairMaterialSeparateChiangInteraction.ior - sinThetaO * sinThetaO) / cosThetaO;
    const float sinGammaT = hairMaterialSeparateChiangInteraction.h / etap;
    const float cosGammaT = ScatterSqrt01(1.0f - sinGammaT * sinGammaT);

    // Compute the transmittance T of a single path through the cylinder.
    const float tmp = -2.0f * cosGammaT / cosThetaT;
    const float3 T = exp(hairMaterialSeparateChiangInteraction.absorptionCoefficient * tmp);

    float3 ap[Hair_Max_Scattering_Events + 1];
    ApSeparateChiang(hairMaterialSeparateChiangInteraction, cosThetaO, T, ap);

    // Compute apPdf from individal ap terms.
    float sumY = 0.0f;
    [unroll]
    for (uint p = 0; p < Hair_Max_Scattering_Events; ++p)
    {
        apPdf[p] = ScatterLuminance(ap[p]);
        sumY += apPdf[p];
    }

    const float invSumY = 1.0f / sumY;
    [unroll]
    for (uint p2 = 0; p2 < Hair_Max_Scattering_Events; ++p2)
    {
        apPdf[p2] *= invSumY;
    }

    apPdf[Hair_Max_Scattering_Events] = 0.0f;
}

#endif

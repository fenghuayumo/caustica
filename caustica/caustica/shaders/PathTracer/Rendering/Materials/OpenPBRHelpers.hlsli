#ifndef __OPENPBR_HELPERS_HLSLI__
#define __OPENPBR_HELPERS_HLSLI__

#include "../../Utils/Math/MathConstants.hlsli"

/** Dielectric F0 from absolute IOR assuming outside IOR = 1. */
float OpenPBRDielectricF0(float ior)
{
    float f = (ior - 1.0f) / max(ior + 1.0f, 1e-4f);
    return f * f;
}

/** Apply OpenPBR specular_weight by modulating the relative dielectric IOR.
    This keeps reflection, transmission, and refraction consistent instead of
    scaling F0 alone. */
float OpenPBRModulatedIor(float relativeIor, float specularWeight)
{
    relativeIor = max(relativeIor, 1e-4f);
    float f0 = OpenPBRDielectricF0(relativeIor);
    float epsilon = sign(relativeIor - 1.0f) * sqrt(saturate(specularWeight * f0));
    epsilon = clamp(epsilon, -0.9999f, 0.9999f);
    return (1.0f + epsilon) / max(1.0f - epsilon, 1e-4f);
}

/** OpenPBR conductor Fresnel using the F82-tint parametrization. */
float3 OpenPBRF82Tint(float3 f0, float3 edgeTint, float cosTheta)
{
    cosTheta = saturate(cosTheta);
    f0 = saturate(f0);
    edgeTint = saturate(edgeTint);
    float3 schlick = f0 + (1.0f - f0) * pow(1.0f - cosTheta, 5.0f);
    const float muBar = 1.0f / 7.0f;
    float3 schlickBar = f0 + (1.0f - f0) * pow(1.0f - muBar, 5.0f);
    float denom = muBar * pow(1.0f - muBar, 6.0f);
    float correction = cosTheta * pow(1.0f - cosTheta, 6.0f) / max(denom, 1e-6f);
    return saturate(schlick - correction * (schlickBar - edgeTint * schlickBar));
}

/** RGB relative IORs from the OpenPBR Abbe/Cauchy parametrization.
    The input eta is incident/transmitted IOR. The green channel is the
    Fraunhofer d-line reference and therefore remains equal to eta. */
float3 OpenPBRDispersionRelativeEta(float eta, float abbeNumber, float dispersionScale)
{
    dispersionScale = saturate(dispersionScale);
    if (dispersionScale <= 0.0f || abbeNumber <= 1e-4f || abs(eta - 1.0f) <= 1e-5f)
        return eta.xxx;

    float nD = eta < 1.0f ? rcp(max(eta, 1e-4f)) : eta;
    float vd = abbeNumber / dispersionScale;
    const float lambdaC = 656.3f;
    const float lambdaD = 587.6f;
    const float lambdaF = 486.1f;
    float invC2 = rcp(lambdaC * lambdaC);
    float invD2 = rcp(lambdaD * lambdaD);
    float invF2 = rcp(lambdaF * lambdaF);
    float B = (nD - 1.0f) / max(vd * (invF2 - invC2), 1e-8f);
    float A = nD - B * invD2;
    float3 wavelengths = float3(650.0f, lambdaD, lambdaF);
    float3 nRgb = A + B / (wavelengths * wavelengths);
    float3 scale = nRgb / max(nD, 1e-4f);
    return eta < 1.0f ? eta / scale : eta * scale;
}

/** OpenPBR coat darkening / absorption attenuation of the coated base.
    Approximates layering: base is attenuated by coat Fresnel coverage and coat_color absorption.
*/
float3 OpenPBRCoatBaseAttenuation(float coatWeight, float3 coatColor, float coatDarkening, float coatIor, float NdotV)
{
    coatWeight = saturate(coatWeight);
    if (coatWeight <= 0.0f)
        return float3(1, 1, 1);

    float F0 = OpenPBRDielectricF0(max(coatIor, 1.0f));
    float F = F0 + (1.0f - F0) * pow(saturate(1.0f - NdotV), 5.0f);
    float coverage = coatWeight * F;

    // Multiple-reflection darkening is absorption-driven. A white coat over a
    // white substrate must remain white in a furnace; coat_darkening therefore
    // cannot introduce gray energy loss by itself.
    float3 coatAbsorption = saturate(coatColor);
    float3 extraDarkening = lerp(1.0f.xxx, coatAbsorption,
        saturate(coatDarkening) * saturate(F0 * 4.0f));
    float3 absorption = lerp(1.0f.xxx, coatAbsorption * extraDarkening, coatWeight);
    return absorption * (1.0f - coverage);
}

/** Cheap thin-film iridescence tint (OpenPBR thin_film_*).
    thickness is in micrometers. Based on a 3-band Airy interference approximation.
*/
float3 OpenPBRThinFilmIridescence(float cosTheta, float thicknessUm, float filmIor, float baseIor)
{
    if (thicknessUm <= 0.0f)
        return float3(1, 1, 1);

    // Convert micrometers to nanometers for wavelength-scale interference.
    float thicknessNm = thicknessUm * 1000.0f;
    float eta = max(filmIor, 1.0f);
    float cosT = sqrt(max(0.0f, 1.0f - (1.0f - cosTheta * cosTheta) / (eta * eta)));
    float path = 2.0f * eta * thicknessNm * cosT;

    // CIE-ish RGB representative wavelengths (nm).
    float3 lambda = float3(650.0f, 550.0f, 450.0f);
    float3 phase = (2.0f * K_PI) * path / lambda;

    // Soften with base/film contrast so zero-weight paths stay neutral.
    float contrast = saturate(abs(eta - max(baseIor, 1.0f)));
    float3 interference = 0.5f + 0.5f * cos(phase);
    return lerp(float3(1, 1, 1), interference, contrast);
}

/** Apply thin-film weight to specular F0. */
float3 OpenPBRApplyThinFilmToF0(float3 specularF0, float thinFilmWeight, float thicknessUm, float filmIor, float baseIor, float NdotV)
{
    thinFilmWeight = saturate(thinFilmWeight);
    if (thinFilmWeight <= 0.0f || thicknessUm <= 0.0f)
        return specularF0;

    float3 irid = OpenPBRThinFilmIridescence(saturate(NdotV), thicknessUm, filmIor, baseIor);
    return lerp(specularF0, saturate(specularF0 * irid + (1.0f - specularF0) * irid * 0.35f), thinFilmWeight);
}

/** Map OpenPBR subsurface radius/color into a homogeneous scattering coefficient. */
float3 OpenPBRSubsurfaceSigmaS(float3 subsurfaceColor, float radius, float3 radiusScale)
{
    float3 meanFreePath = max(radius * max(radiusScale, 0.0f), 1e-4f);
    float3 albedo = saturate(subsurfaceColor);
    // Higher albedo -> more scattering relative to absorption.
    return albedo / meanFreePath;
}

/** Map transmission_scatter into sigmaS. */
float3 OpenPBRTransmissionSigmaS(float3 transmissionScatter, float depth)
{
    float3 scatter = max(transmissionScatter, 0.0f);
    if (all(scatter <= 0.0f))
        return float3(0, 0, 0);
    float d = max(depth, 1e-3f);
    return scatter / d;
}

#endif

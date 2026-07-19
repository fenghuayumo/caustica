#ifndef __OPENPBR_HELPERS_HLSLI__
#define __OPENPBR_HELPERS_HLSLI__

#include "../../Utils/Math/MathConstants.hlsli"

/** Dielectric F0 from absolute IOR assuming outside IOR = 1. */
float OpenPBRDielectricF0(float ior)
{
    float f = (ior - 1.0f) / max(ior + 1.0f, 1e-4f);
    return f * f;
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

    // Physical darkening from multiple internal reflections; coat_darkening=0 undoes it artistically.
    float darken = lerp(1.0f, saturate(1.0f - 0.2f * F0), saturate(coatDarkening));
    float3 absorption = lerp(float3(1, 1, 1), saturate(coatColor) * darken, coatWeight);
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

/** Abbe-number dispersion: returns per-channel relative eta scale for RGB.
    OpenPBR: transmission_dispersion_abbe_number (Vd), transmission_dispersion_scale.
*/
float3 OpenPBRDispersionEtaScale(float abbeNumber, float dispersionScale)
{
    dispersionScale = saturate(dispersionScale);
    if (dispersionScale <= 0.0f || abbeNumber <= 1e-3f)
        return float3(1, 1, 1);

    // nF - nC = (n_d - 1) / Vd; distribute across R/G/B around the medium IOR.
    float delta = dispersionScale / max(abbeNumber, 1e-3f);
    // R (long) lower IOR, B (short) higher IOR.
    return float3(1.0f - 0.5f * delta, 1.0f, 1.0f + 0.5f * delta);
}

/** Map OpenPBR subsurface radius/color into a homogeneous scattering coefficient. */
float3 OpenPBRSubsurfaceSigmaS(float3 subsurfaceColor, float radius, float scale)
{
    float meanFreePath = max(radius * max(scale, 0.0f), 1e-4f);
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

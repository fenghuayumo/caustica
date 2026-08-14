#ifndef __STANDARD_BSDF_HLSLI__ // using instead of "#pragma once" due to https://github.com/microsoft/DirectXShaderCompiler/issues/3943
#define __STANDARD_BSDF_HLSLI__

#include "../../Config.h"    

#include "../../Utils/Math/MathConstants.hlsli"
#include "../../Utils/Math/MathHelpers.hlsli"

#include "IBSDF.hlsli"
#include "BxDF.hlsli"
#include "../../../ThirdParty/RTXCR/HairChiangBSDF.hlsli"
#include "../../../ThirdParty/RTXCR/HairFarFieldBCSDF.hlsli"

static const uint CausticaHairModel_FarField = 0;
static const uint CausticaHairModel_Chiang = 1;

RTXCR_HairMaterialData CausticaMakeHairMaterialData(const StandardBSDFData data)
{
    RTXCR_HairMaterialData material;
    material.baseColor = data.HairBaseColor();
    material.longitudinalRoughness = max((float)data.HairLongitudinalRoughness(), 0.001f);
    material.azimuthalRoughness = max((float)data.HairAzimuthalRoughness(), 0.001f);
    material.ior = max((float)data.HairIor(), 1.001f);
    material.eta = 1.0f / material.ior;
    material.fresnelApproximation = 1;
    material.absorptionModel = RTXCR_HairAbsorptionModel_Physics;
    material.melanin = saturate((float)data.HairMelanin());
    material.melaninRedness = saturate((float)data.HairMelaninRedness());
    material.cuticleAngleInDegrees = (float)data.HairCuticleAngle();
    return material;
}

RTXCR_HairInteractionSurface CausticaMakeHairInteraction(const ShadingData shadingData)
{
    RTXCR_HairInteractionSurface surface;
    // RTXCR's local hair convention is X=strand tangent, Z=fiber normal.
    surface.incidentRayDirection = float3(
        dot(shadingData.V, shadingData.T),
        dot(shadingData.V, shadingData.B),
        dot(shadingData.V, shadingData.N));
    surface.shadingNormal = float3(0.f, 0.f, 1.f);
    surface.tangent = float3(1.f, 0.f, 0.f);
    return surface;
}

float3 CausticaHairToLocal(const ShadingData shadingData, float3 direction)
{
    return float3(dot(direction, shadingData.T), dot(direction, shadingData.B), dot(direction, shadingData.N));
}

float3 CausticaHairFromLocal(const ShadingData shadingData, float3 direction)
{
    return shadingData.T * direction.x + shadingData.B * direction.y + shadingData.N * direction.z;
}

// Evaluate the same directional mixture PDF used by RTXCR_SampleChiangBsdf().
// Keeping this separate from the BCSDF value is required for unbiased NEE/MIS.
float CausticaHairChiangPdf(
    const RTXCR_HairMaterialInteraction interaction, const float3 wi, const float3 wo)
{
    const float sinThetaO = wo.x;
    const float cosThetaO = RTXCR_Sqrt01(1.f - sinThetaO * sinThetaO);
    const float phiO = RTXCR_Atan2safe(wo.z, wo.y);
    const float sinThetaI = wi.x;
    const float cosThetaI = RTXCR_Sqrt01(1.f - sinThetaI * sinThetaI);
    const float phiI = RTXCR_Atan2safe(wi.z, wi.y);
    const float dphi = phiI - phiO;

    float apPdf[RTXCR_Hair_Max_Scattering_Events + 1];
    RTXCR_ComputeApPdf(interaction, cosThetaO, apPdf);

    const float etap = RTXCR_Sqrt0(interaction.ior * interaction.ior - sinThetaO * sinThetaO) /
        max(cosThetaO, 1e-6f);
    const float sinGammaT = interaction.h / max(etap, 1e-6f);
    const float gammaT = asin(clamp(sinGammaT, -1.f, 1.f));

    float pdf = 0.f;
    [unroll]
    for (uint p = 0; p < RTXCR_Hair_Max_Scattering_Events; ++p)
    {
        float sinThetaIp;
        float cosThetaIp;
        if (p == 0)
        {
            sinThetaIp = sinThetaI * interaction.cos2kAlpha[1] - cosThetaI * interaction.sin2kAlpha[1];
            cosThetaIp = cosThetaI * interaction.cos2kAlpha[1] + sinThetaI * interaction.sin2kAlpha[1];
        }
        else if (p == 1)
        {
            sinThetaIp = sinThetaI * interaction.cos2kAlpha[0] + cosThetaI * interaction.sin2kAlpha[0];
            cosThetaIp = cosThetaI * interaction.cos2kAlpha[0] - sinThetaI * interaction.sin2kAlpha[0];
        }
        else
        {
            sinThetaIp = sinThetaI * interaction.cos2kAlpha[2] + cosThetaI * interaction.sin2kAlpha[2];
            cosThetaIp = cosThetaI * interaction.cos2kAlpha[2] - sinThetaI * interaction.sin2kAlpha[2];
        }

        pdf += RTXCR_MP(abs(cosThetaIp), cosThetaO, sinThetaIp, sinThetaO, interaction.v[p]) *
            apPdf[p] * RTXCR_NP(dphi, p, interaction.logisticDistributionScalar, interaction.gammaI, gammaT);
    }

    pdf += RTXCR_MP(cosThetaI, cosThetaO, sinThetaI, sinThetaO,
        interaction.v[RTXCR_Hair_Max_Scattering_Events]) *
        apPdf[RTXCR_Hair_Max_Scattering_Events] * RTXCR_ONE_OVER_TWO_PI;
    return max(pdf, 0.f);
}

void CausticaEvalHair(
    const StandardBSDFData data, const ShadingData shadingData, const float3 wo,
    out float3 value, out float pdf, out float diffuseWeight)
{
    const RTXCR_HairMaterialData material = CausticaMakeHairMaterialData(data);
    const RTXCR_HairInteractionSurface surface = CausticaMakeHairInteraction(shadingData);
    const float3 wiLocal = surface.incidentRayDirection;
    const float3 woLocal = CausticaHairToLocal(shadingData, wo);
    diffuseWeight = 0.f;

    if (data.HairModel() == CausticaHairModel_Chiang)
    {
        const RTXCR_HairMaterialInteraction interaction = RTXCR_CreateHairMaterialInteraction(material, surface);
        value = RTXCR_HairChiangBsdfEval(interaction, woLocal, wiLocal);
        pdf = CausticaHairChiangPdf(interaction, woLocal, wiLocal);
    }
    else
    {
        const RTXCR_HairMaterialInteractionBcsdf interaction =
            RTXCR_CreateHairMaterialInteractionBcsdf(
                material, data.HairDiffuseReflectionTint(),
                data.HairDiffuseReflectionWeight(), material.longitudinalRoughness);
        float3 specular;
        float3 diffuse;
        RTXCR_HairFarFieldBcsdfEval(surface, interaction, woLocal, wiLocal, specular, diffuse, pdf);
        value = specular + diffuse;
        diffuseWeight = saturate((float)data.HairDiffuseReflectionWeight());
    }
}

/** Implementation of Falcor's standard surface BSDF.

    The BSDF has the following lobes:
    - Delta reflection (ideal specular reflection).
    - Specular reflection using a GGX microfacet model.
    - Diffuse reflection using Disney's diffuse BRDF.
    - Delta transmission (ideal specular transmission).
    - Specular transmission using a GGX microfacet model.
    - Diffuse transmission.

    The BSDF is a linear combination of the above lobes.
*/
struct StandardBSDF // : IBSDF
{
#if RecycleSelectSamples
    static const int cRandomNumberCountForSampling = 3;
#else
    static const int cRandomNumberCountForSampling = 4;
#endif

    StandardBSDFData data;      ///< BSDF parameters.
    // float3 emission;            ///< Radiance emitted in the incident direction (wi).

    static StandardBSDF make( StandardBSDFData data ) 
    { 
        StandardBSDF d;
        d.data = data;
        //d.emission = emission;
        return d;
    }

    float4 eval(const ShadingData shadingData, const float3 wo)
    {
        if (data.IsHair())
        {
            float3 value;
            float pdf;
            float diffuseWeight;
            CausticaEvalHair(data, shadingData, wo, value, pdf, diffuseWeight);
            return float4(value, Average(value) * (1.f - diffuseWeight));
        }

        float3 wiLocal = shadingData.toLocal(shadingData.V);
        float3 woLocal = shadingData.toLocal(wo);

        FalcorBSDF bsdf = FalcorBSDF::make(shadingData, data);

        return bsdf.eval(wiLocal, woLocal);
    }

    bool sample(const ShadingData shadingData, const float4 preGeneratedSamples, out BSDFSample result, bool useImportanceSampling)
    {
        if (data.IsHair())
        {
            const RTXCR_HairMaterialData material = CausticaMakeHairMaterialData(data);
            const RTXCR_HairInteractionSurface surface = CausticaMakeHairInteraction(shadingData);
            const float3 wiLocal = surface.incidentRayDirection;
            float3 woLocal = 0.f;
            float3 value = 0.f;
            float pdf = 0.f;
            bool valid = false;
            if (data.HairModel() == CausticaHairModel_Chiang)
            {
                float2 u[2] = {
                    preGeneratedSamples.xy,
                    float2(preGeneratedSamples.z, frac(preGeneratedSamples.x + preGeneratedSamples.y * 0.61803398875f))
                };
                RTXCR_HairLobeType hairLobe;
                const RTXCR_HairMaterialInteraction interaction = RTXCR_CreateHairMaterialInteraction(material, surface);
                valid = RTXCR_SampleChiangBsdf(interaction, wiLocal, u, woLocal, pdf, value, hairLobe);
            }
            else
            {
                const RTXCR_HairMaterialInteractionBcsdf interaction =
                    RTXCR_CreateHairMaterialInteractionBcsdf(
                        material, data.HairDiffuseReflectionTint(),
                        data.HairDiffuseReflectionWeight(), material.longitudinalRoughness);
                float2 u[2] = {
                    preGeneratedSamples.xy,
                    float2(frac(preGeneratedSamples.y + preGeneratedSamples.z * 0.754877666f),
                           frac(preGeneratedSamples.x * 0.569840296f + preGeneratedSamples.z))
                };
                float3 specular;
                float3 diffuse;
                const float h = 2.f * preGeneratedSamples.z - 1.f;
                valid = RTXCR_SampleFarFieldBcsdf(surface, interaction, wiLocal, h,
                    frac(preGeneratedSamples.x + preGeneratedSamples.y + preGeneratedSamples.z),
                    u, woLocal, specular, diffuse, pdf);
                value = specular + diffuse;
            }
            result.wo = CausticaHairFromLocal(shadingData, woLocal);
            result.pdf = pdf;
            result.weight = pdf > 0.f ? value / pdf : 0.f;
            result.lobe = (uint)LobeType::SpecularReflection;
            result.lobeP = 1.f;
            return valid && pdf > 0.f && all(isfinite(result.weight));
        }

        if (!useImportanceSampling) return sampleReference(shadingData, preGeneratedSamples, result);

        float3 wiLocal = shadingData.toLocal(shadingData.V);
        float3 woLocal = float3(0,0,0);

        FalcorBSDF bsdf = FalcorBSDF::make(shadingData, data);
#if RecycleSelectSamples
        bool valid = bsdf.sample(wiLocal, woLocal, result.pdf, result.weight, result.lobe, result.lobeP, preGeneratedSamples.xyz);
#else
        bool valid = bsdf.sample(wiLocal, woLocal, result.pdf, result.weight, result.lobe, result.lobeP, preGeneratedSamples.xyzw);
#endif
        result.wo = shadingData.fromLocal(woLocal);

        return valid;
    }

    float evalPdf(const ShadingData shadingData, const float3 wo, bool useImportanceSampling)
    {
        if (data.IsHair())
        {
            float3 value;
            float pdf;
            float diffuseWeight;
            CausticaEvalHair(data, shadingData, wo, value, pdf, diffuseWeight);
            return max(pdf, 0.f);
        }

        if (!useImportanceSampling) return evalPdfReference(shadingData, wo);

        float3 wiLocal = shadingData.toLocal(shadingData.V);
        float3 woLocal = shadingData.toLocal(wo);

        FalcorBSDF bsdf = FalcorBSDF::make(shadingData, data);

        return bsdf.evalPdf(wiLocal, woLocal);
    }

    void estimateSpecDiffBSDF( out float3 outDiffEstimate, out float3 outSpecEstimate, const float3 normal, const float3 viewVector )
    {
        if (data.IsHair())
        {
            const float diffuseWeight = saturate((float)data.HairDiffuseReflectionWeight());
            outDiffEstimate = data.HairDiffuseReflectionTint() * diffuseWeight;
            outSpecEstimate = data.HairBaseColor() * (1.f - diffuseWeight);
            return;
        }
    #if 1
        lpfloat dataRoughness = data.Roughness();
        float alpha = dataRoughness * dataRoughness;
        float roughness = alpha < kMinGGXAlpha ? 0.f : dataRoughness;

        // Compute approximation of the albedos.
        // For now use the blend weights and colors, but this should be improved to better numerically approximate the integrals.
        lpfloat dataDiffuseTransmission = data.DiffuseTransmission();
        lpfloat dataSpecularTransmission = data.SpecularTransmission();
        lpfloat3 dataTransmission = data.Transmission();
        lpfloat3 dataSpecular = data.Specular();
        lpfloat3 diffuseReflectionAlbedo = (lpfloat(1.f) - dataDiffuseTransmission) * (lpfloat(1.f) - dataSpecularTransmission) * data.Diffuse();
        lpfloat3 diffuseTransmissionAlbedo = dataDiffuseTransmission * dataTransmission * (lpfloat(1.f) - dataSpecularTransmission); // used to have  "* (1.f - dataSpecularTransmission)" too
        lpfloat3 specularReflectionAlbedo = (lpfloat(1.f) - dataSpecularTransmission) * dataSpecular;
        lpfloat3 specularTransmissionAlbedo = dataSpecularTransmission * dataTransmission;

        // Note - not clamping estimate here - it can be zero; clamp it at use location
        outDiffEstimate = diffuseReflectionAlbedo+diffuseTransmissionAlbedo + data.FuzzColor() * data.FuzzWeight(); // note, also adding base path throughput to modulation here!
        const float NdotV = saturate(dot(normal, viewVector));
        const float ggxAlpha = roughness * roughness;
        float3 specularReflectance = approxSpecularIntegralGGX(specularReflectionAlbedo, ggxAlpha, NdotV); // note, also adding base path throughput to modulation here!
        specularReflectance += specularTransmissionAlbedo; // best approximation for now
        outSpecEstimate = specularReflectance;
     #else
        outDiffEstimate = float3(1,1,1);
        outSpecEstimate = float3(1,1,1);
     #endif
    }

    #if 0
    BSDFProperties getProperties(const ShadingData shadingData)
    {
        BSDFProperties p; p.flags = 0; // = {};

        // p.emission = emission;

        // Clamp roughness so it's representable of what is actually used in FalcorBSDF.
        // Roughness^2 below kMinGGXAlpha is used to indicate perfectly smooth surfaces.
        lpfloat dataRoughness = data.Roughness();
        float alpha = dataRoughness * dataRoughness;
        p.roughness = alpha < kMinGGXAlpha ? 0.f : dataRoughness;


        // Compute approximation of the albedos.
        // For now use the blend weights and colors, but this should be improved to better numerically approximate the integrals.
        lpfloat dataDiffuseTransmission = data.DiffuseTransmission();
        lpfloat dataSpecularTransmission = data.SpecularTransmission();
        lpfloat3 dataTransmission = data.Transmission();
        lpfloat3 dataSpecular = data.Specular();
        p.diffuseReflectionAlbedo = (1.f - dataDiffuseTransmission) * (1.f - dataSpecularTransmission) * data.Diffuse() + data.FuzzColor() * data.FuzzWeight();
        p.diffuseTransmissionAlbedo = dataDiffuseTransmission * dataTransmission * (1.f - dataSpecularTransmission); // used to have  "* (1.f - dataSpecularTransmission)" too
        p.specularReflectionAlbedo = (1.f - dataSpecularTransmission) * dataSpecular;
        p.specularTransmissionAlbedo = dataSpecularTransmission * dataTransmission;

        // Pass on our specular reflectance field unmodified.
        p.specularReflectance = dataSpecular;

        if (dataDiffuseTransmission > 0.f || dataSpecularTransmission > 0.f) p.flags |= (uint)BSDFProperties::Flags::IsTransmissive;

        return p;
    }
    #endif

    uint getLobes(const ShadingData shadingData)
    {
        if (data.IsHair())
            return (uint)LobeType::SpecularReflection
                | (data.HairDiffuseReflectionWeight() > 0.f ? (uint)LobeType::DiffuseReflection : 0u);
        return FalcorBSDF::getLobes(data);
    }


    // Additional functions

    /** Reference implementation that uses cosine-weighted hemisphere sampling.
        This is for testing purposes only.
        \param[in] sd Shading data.
        \param[in] sampleGenerator Sample generator.
        \param[out] result Generated sample. Only valid if true is returned.
        \return True if a sample was generated, false otherwise.
    */
    bool sampleReference(const ShadingData shadingData, const float4 preGeneratedSamples, out BSDFSample result)
    {
        const bool isTransmissive = (getLobes(shadingData) & (uint)LobeType::Transmission) != 0;

        float3 wiLocal = shadingData.toLocal(shadingData.V);
        float3 woLocal = sample_cosine_hemisphere_concentric(preGeneratedSamples.xy, result.pdf); // pdf = cos(theta) / pi

        if (isTransmissive)
        {
            if (preGeneratedSamples.z < 0.5f)
            {
                woLocal.z = -woLocal.z;
            }
            result.pdf *= 0.5f;
            if (min(abs(wiLocal.z), abs(woLocal.z)) < kMinCosTheta || result.pdf == 0.f) return false;
        }
        else
        {
            if (min(wiLocal.z, woLocal.z) < kMinCosTheta || result.pdf == 0.f) return false;
        }

        FalcorBSDF bsdf = FalcorBSDF::make(shadingData, data);

        result.wo = shadingData.fromLocal(woLocal);
        result.weight = (bsdf.eval(wiLocal, woLocal).rgb) / result.pdf;
        result.lobe = (uint)(woLocal.z > 0.f ? (uint)LobeType::DiffuseReflection : (uint)LobeType::DiffuseTransmission);

        return true;
    }

    /** Evaluates the directional pdf for sampling the given direction using the reference implementation.
        \param[in] sd Shading data.
        \param[in] wo Outgoing direction.
        \return PDF with respect to solid angle for sampling direction wo.
    */
    float evalPdfReference(const ShadingData shadingData, const float3 wo)
    {
        const bool isTransmissive = (getLobes(shadingData) & (uint)LobeType::Transmission) != 0;

        float3 wiLocal = shadingData.toLocal(shadingData.V);
        float3 woLocal = shadingData.toLocal(wo);

        if (isTransmissive)
        {
            if (min(abs(wiLocal.z), abs(woLocal.z)) < kMinCosTheta) return 0.f;
            return 0.5f * woLocal.z * K_1_PI; // pdf = 0.5 * cos(theta) / pi
        }
        else
        {
            if (min(wiLocal.z, woLocal.z) < kMinCosTheta) return 0.f;
            return woLocal.z * K_1_PI; // pdf = cos(theta) / pi
        }
    }

    void evalDeltaLobes(const ShadingData shadingData, out DeltaLobe deltaLobes[cMaxDeltaLobes], out int deltaLobeCount, out float nonDeltaPart)
    {
        if (data.IsHair())
        {
            [unroll] for (uint i = 0; i < cMaxDeltaLobes; ++i) deltaLobes[i] = DeltaLobe::make();
            deltaLobeCount = 0;
            nonDeltaPart = 1.f;
            return;
        }
        float3 wiLocal = shadingData.toLocal(shadingData.V);
        
        FalcorBSDF bsdf = FalcorBSDF::make(shadingData, data); 
        bsdf.evalDeltaLobes(wiLocal, deltaLobes, deltaLobeCount, nonDeltaPart);
        
        // local to world!
        for ( uint i = 0; i < deltaLobeCount; i++ )
            deltaLobes[i].dir = shadingData.fromLocal(deltaLobes[i].dir);
    }


};

#endif // __STANDARD_BSDF_HLSLI__

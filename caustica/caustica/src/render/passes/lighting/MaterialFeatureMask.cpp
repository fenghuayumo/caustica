#include <render/passes/lighting/materialFeatureMask.h>

#include <render/passes/lighting/MaterialGpuCache.h>

#include <shaders/PathTracer/Materials/MaterialPT.h>

#include <array>
#include <cassert>

namespace caustica::render
{

namespace
{
    constexpr MaterialFeatureTierDesc kMaterialFeatureTiers[] = {
        { 0, 0, materialFeatureMask(MaterialFeature::Specialized), "Ubershader" },
        { 1, materialFeatureMask(MaterialFeature::Specialized), 0, "Standard" },
        { 2, materialFeatureMask(MaterialFeature::Specialized) | materialFeatureMask(MaterialFeature::NonEmissive) | materialFeatureMask(MaterialFeature::NonAnalyticProxy),
          materialFeatureMask(MaterialFeature::HasTransmission) | materialFeatureMask(MaterialFeature::ThinSurface) | materialFeatureMask(MaterialFeature::UseNormalTexture)
              | materialFeatureMask(MaterialFeature::AlphaTest) | materialFeatureMask(MaterialFeature::OnlyDeltaLobes),
          "NonEmissive" },
        { 3, materialFeatureMask(MaterialFeature::Specialized) | materialFeatureMask(MaterialFeature::HasTransmission), 0, "Transmission" },
        { 4, materialFeatureMask(MaterialFeature::Specialized) | materialFeatureMask(MaterialFeature::ThinSurface), 0, "ThinSurface" },
        { 5, materialFeatureMask(MaterialFeature::Specialized) | materialFeatureMask(MaterialFeature::UseNormalTexture), 0, "NormalMap" },
        { 6, materialFeatureMask(MaterialFeature::Specialized) | materialFeatureMask(MaterialFeature::AlphaTest), 0, "AlphaTest" },
        { 7, materialFeatureMask(MaterialFeature::Specialized) | materialFeatureMask(MaterialFeature::OnlyDeltaLobes), 0, "DeltaLobes" },
    };

    static_assert(std::size(kMaterialFeatureTiers) == kMaterialFeatureTierCount, "kMaterialFeatureTierCount must match tier table size");

    bool TierMatchesMask(const MaterialFeatureTierDesc& tier, materialFeatureMask mask)
    {
        if ((mask & tier.requiredMask) != tier.requiredMask)
            return false;
        if ((mask & tier.forbiddenMask) != 0)
            return false;
        return true;
    }

    uint32_t ScoreTierMatch(const MaterialFeatureTierDesc& tier, materialFeatureMask mask)
    {
        if (!TierMatchesMask(tier, mask))
            return 0;

        uint32_t score = 0;
        for (uint32_t bit = 0; bit < 32; ++bit)
        {
            const materialFeatureMask featureBit = (1u << bit);
            if ((tier.requiredMask & featureBit) != 0 && (mask & featureBit) != 0)
                ++score;
        }
        return score;
    }
}

materialFeatureMask computeMaterialFeatureMask(const PTMaterial& material)
{
    materialFeatureMask mask = materialFeatureMask(MaterialFeature::Specialized);

    const bool isEmissive = material.isEmissive();
    const bool isAnalyticProxy = material.enableAsAnalyticLightProxy;

    if (!isEmissive)
        mask = mask | MaterialFeature::NonEmissive;
    if (!isAnalyticProxy)
        mask = mask | MaterialFeature::NonAnalyticProxy;

#if 0 // keep disabled until shader-side tiers are validated
    PTMaterialData data{};
    material.fillData(data);

    if (material.enableTransmission)
        mask = mask | MaterialFeature::HasTransmission;

    if ((data.Flags & PTMaterialFlags_ThinSurface) != 0)
        mask = mask | MaterialFeature::ThinSurface;

    if ((data.Flags & PTMaterialFlags_UseNormalTexture) != 0)
        mask = mask | MaterialFeature::UseNormalTexture;

    if (material.enableAlphaTesting)
        mask = mask | MaterialFeature::AlphaTest;

    static const float kMinGGXRoughness = 0.08f;
    const bool onlyDeltaLobes =
        ((material.enableTransmission && material.transmissionFactor == 1.0f) || (material.metalness == 1.0f))
        && (material.roughness < kMinGGXRoughness)
        && ((data.Flags & PTMaterialFlags_UseMetalRoughOrSpecularTexture) == 0);
    if (onlyDeltaLobes)
        mask = mask | MaterialFeature::OnlyDeltaLobes;
#endif

    return mask;
}

uint32_t mapFeatureMaskToTier(materialFeatureMask mask)
{
    if (!hasFeature(mask, MaterialFeature::Specialized))
        return 0;

    uint32_t bestTier = 1;
    uint32_t bestScore = 0;
    for (const MaterialFeatureTierDesc& tier : kMaterialFeatureTiers)
    {
        if (tier.tierIndex == 0)
            continue;

        const uint32_t score = ScoreTierMatch(tier, mask);
        if (score > bestScore)
        {
            bestScore = score;
            bestTier = tier.tierIndex;
        }
    }

    return bestTier;
}

const MaterialFeatureTierDesc& getMaterialFeatureTierDesc(uint32_t tierIndex)
{
    assert(tierIndex < kMaterialFeatureTierCount);
    return kMaterialFeatureTiers[tierIndex];
}

std::vector<caustica::ShaderMacro> buildMaterialShaderMacros(uint32_t tierIndex)
{
    assert(tierIndex < kMaterialFeatureTierCount);

    std::vector<caustica::ShaderMacro> macros;
    macros.reserve(6);
    macros.push_back({ "CAUSTICA_MATERIAL_FEATURE_TIER", std::to_string(tierIndex) });

    if (tierIndex == 0)
    {
        macros.push_back({ "CAUSTICA_MATERIAL_PERMUTATIONS_ENABLED", "0" });
        return macros;
    }

    macros.push_back({ "CAUSTICA_MATERIAL_PERMUTATIONS_ENABLED", "1" });

    const MaterialFeatureTierDesc& tier = getMaterialFeatureTierDesc(tierIndex);
    if (hasFeature(tier.requiredMask, MaterialFeature::NonEmissive))
        macros.push_back({ "CAUSTICA_MATERIAL_IS_EMISSIVE", "0" });
    if (hasFeature(tier.requiredMask, MaterialFeature::NonAnalyticProxy))
        macros.push_back({ "CAUSTICA_MATERIAL_IS_ANALYTIC_LIGHT_PROXY", "0" });
    if (hasFeature(tier.requiredMask, MaterialFeature::HasTransmission))
        macros.push_back({ "CAUSTICA_MATERIAL_HAS_TRANSMISSION", "1" });
    if (hasFeature(tier.requiredMask, MaterialFeature::ThinSurface))
        macros.push_back({ "CAUSTICA_MATERIAL_THIN_SURFACE", "1" });
    if (hasFeature(tier.requiredMask, MaterialFeature::UseNormalTexture))
        macros.push_back({ "CAUSTICA_MATERIAL_USE_NORMAL_TEXTURE", "1" });
    if (hasFeature(tier.requiredMask, MaterialFeature::AlphaTest))
        macros.push_back({ "CAUSTICA_MATERIAL_ALPHA_TEST", "1" });
    if (hasFeature(tier.requiredMask, MaterialFeature::OnlyDeltaLobes))
        macros.push_back({ "CAUSTICA_MATERIAL_ONLY_DELTA_LOBES", "1" });

    return macros;
}

std::string materialFeatureTierStableName(uint32_t tierIndex)
{
    assert(tierIndex < kMaterialFeatureTierCount);
    return std::string(getMaterialFeatureTierDesc(tierIndex).debugName);
}

} // namespace caustica::render

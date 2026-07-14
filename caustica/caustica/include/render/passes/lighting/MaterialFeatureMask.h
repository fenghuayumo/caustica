#pragma once

#include <assets/loader/ShaderFactory.h>

#include <cstdint>
#include <string_view>
#include <vector>

struct PTMaterial;

namespace caustica::render
{

using materialFeatureMask = uint32_t;

// Shader-relevant material features. Each bit corresponds to an optional specialization
// path in the path-tracing ClosestHit/AnyHit library. Runtime material parameters that
// do not change generated code belong in PTMaterialData, not here.
enum class MaterialFeature : materialFeatureMask
{
    None              = 0,

    // When clear, compile the ubershader path (CAUSTICA_MATERIAL_PERMUTATIONS_ENABLED=0).
    Specialized       = 1u << 0,

    // Strip emissive MIS / sampling paths when the material cannot emit light.
    NonEmissive       = 1u << 1,

    // Strip analytic light proxy paths when the material is not used as a proxy.
    NonAnalyticProxy  = 1u << 2,

    // Reserved for future tier expansion. Disabled in the current bake path (#if 0 in PTMaterial).
    HasTransmission   = 1u << 3,
    ThinSurface       = 1u << 4,
    UseNormalTexture  = 1u << 5,
    AlphaTest         = 1u << 6,
    OnlyDeltaLobes    = 1u << 7,
};

constexpr materialFeatureMask operator|(MaterialFeature a, MaterialFeature b)
{
    return materialFeatureMask(a) | materialFeatureMask(b);
}

constexpr materialFeatureMask operator|(materialFeatureMask a, MaterialFeature b)
{
    return a | materialFeatureMask(b);
}

constexpr materialFeatureMask operator&(materialFeatureMask a, MaterialFeature b)
{
    return a & materialFeatureMask(b);
}

inline bool hasFeature(materialFeatureMask mask, MaterialFeature feature)
{
    const materialFeatureMask featureBit = materialFeatureMask(feature);
    return (mask & featureBit) == featureBit;
}

struct MaterialFeatureTierDesc
{
    uint32_t                tierIndex = 0;
    materialFeatureMask     requiredMask = 0;
    materialFeatureMask     forbiddenMask = 0;
    std::string_view        debugName;
};

// Number of offline-compilable material tiers. Keep in sync with PathTracerShaders.cfg.
constexpr uint32_t kMaterialFeatureTierCount = 8;

[[nodiscard]] materialFeatureMask computeMaterialFeatureMask(const PTMaterial& material);

// Maps a feature mask to the closest supported offline tier.
[[nodiscard]] uint32_t mapFeatureMaskToTier(materialFeatureMask mask);

[[nodiscard]] const MaterialFeatureTierDesc& getMaterialFeatureTierDesc(uint32_t tierIndex);

// Macros passed to DXC / ShaderMake for a tier. Includes CAUSTICA_MATERIAL_FEATURE_TIER.
[[nodiscard]] std::vector<caustica::ShaderMacro> buildMaterialShaderMacros(uint32_t tierIndex);

[[nodiscard]] std::string materialFeatureTierStableName(uint32_t tierIndex);

} // namespace caustica::render

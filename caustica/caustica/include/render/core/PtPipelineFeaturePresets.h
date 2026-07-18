#pragma once

#include <assets/loader/ShaderMacro.h>

#include <cstdint>
#include <string_view>
#include <vector>

struct PathTracerSettings;

namespace caustica::render
{

// Closed feature-preset matrix. Offline cook (cook_shaders.py) produces shader *libraries*
// for this matrix. Runtime snaps to the nearest cooked preset, loads those libraries, and
// CreateStateObjects the active preset's REF/BUILD/FILL bundle on first use (UE-style).
// Includes curated multi-feature combos — not a full combinatorial grid.
enum class PtFeaturePresetId : uint32_t
{
    Default = 0,

    // Single-axis
    ReSTIR_DI,
    ReSTIR_GI,
    ReSTIR_PT,
    OMM_On,
    NEE_Off,
    RR_Off,
    Fp32Types,
    LD_Off,
    Firefly_Off,
    ApproxMIS_Off,
    BakedEnv_On,
    NEE_Off_BakedEnv,
    NEE_Candidates_8,
    StablePlanes_1,
    NestedQuality_2,

    // Curated combos (common editor / realtime paths)
    ReSTIR_DI_OMM,
    ReSTIR_GI_OMM,
    ReSTIR_PT_OMM,
    ReSTIR_DI_BakedEnv,
    ReSTIR_GI_BakedEnv,
    ReSTIR_PT_BakedEnv,
    ReSTIR_DI_OMM_BakedEnv,
    OMM_BakedEnv,
    ReSTIR_DI_NEE8,
    ReSTIR_DI_StablePlanes_1,

    Count
};

struct PtFeaturePresetResolveInput
{
    const PathTracerSettings* settings = nullptr;
    bool useOpacityMicromaps = false;
    bool sampleBakedEnvironment = false;
};

[[nodiscard]] constexpr uint32_t ptFeaturePresetCount()
{
    return static_cast<uint32_t>(PtFeaturePresetId::Count);
}

[[nodiscard]] std::string_view ptFeaturePresetName(PtFeaturePresetId id);

// Exact cooked macro list for a preset (insertion order matches Python cook).
void fillPtFeaturePresetMacros(PtFeaturePresetId id, std::vector<caustica::ShaderMacro>& macros);

// Ideal macros implied by live settings (may not equal any cooked preset).
void fillDesiredPtFeatureMacros(
    const PtFeaturePresetResolveInput& input,
    std::vector<caustica::ShaderMacro>& macros);

// Nearest cooked preset by weighted macro distance (prefers curated combos over single-axis snap).
[[nodiscard]] PtFeaturePresetId resolvePtFeaturePreset(const PtFeaturePresetResolveInput& input);

} // namespace caustica::render

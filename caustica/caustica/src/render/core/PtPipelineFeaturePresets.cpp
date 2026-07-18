#include <render/core/PtPipelineFeaturePresets.h>

#include <render/core/PathTracerSettings.h>

#include <shaders/PathTracer/Lighting/LightingTypes.hlsli>

#include <cassert>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>

namespace caustica::render
{
namespace
{

constexpr const char* kPresetNames[] = {
    "Default",
    "ReSTIR_DI",
    "ReSTIR_GI",
    "ReSTIR_PT",
    "OMM_On",
    "NEE_Off",
    "RR_Off",
    "Fp32Types",
    "LD_Off",
    "Firefly_Off",
    "ApproxMIS_Off",
    "BakedEnv_On",
    "NEE_Off_BakedEnv",
    "NEE_Candidates_8",
    "StablePlanes_1",
    "NestedQuality_2",
    "ReSTIR_DI_OMM",
    "ReSTIR_GI_OMM",
    "ReSTIR_PT_OMM",
    "ReSTIR_DI_BakedEnv",
    "ReSTIR_GI_BakedEnv",
    "ReSTIR_PT_BakedEnv",
    "ReSTIR_DI_OMM_BakedEnv",
    "OMM_BakedEnv",
    "ReSTIR_DI_NEE8",
    "ReSTIR_DI_StablePlanes_1",
};

static_assert(std::size(kPresetNames) == ptFeaturePresetCount());

// Keep in sync with support/python/precompile_pt_shader_bins.py::base_global_macro_map
void fillBaseMacros(std::vector<caustica::ShaderMacro>& macros)
{
    macros.clear();
    macros.push_back({ "ENABLE_DEBUG_SURFACE_VIZ", "0" });
    macros.push_back({ "ENABLE_DEBUG_LINES_VIZ", "0" });
    macros.push_back({ "USE_NVAPI_HIT_OBJECT_EXTENSION", "0" });
    macros.push_back({ "USE_NVAPI_REORDER_THREADS", "0" });
    macros.push_back({ "USE_DX_HIT_OBJECT_EXTENSION", "0" });
    macros.push_back({ "USE_DX_MAYBE_REORDER_THREADS", "0" });
    macros.push_back({ "PT_ENABLE_RUSSIAN_ROULETTE", "1" });
    macros.push_back({ "PT_NEE_ENABLED", "1" });
    macros.push_back({ "PT_USE_RESTIR_DI", "0" });
    macros.push_back({ "PT_USE_RESTIR_GI", "0" });
    macros.push_back({ "PT_USE_RESTIR_PT", "0" });
    macros.push_back({ "CAUSTICA_ENABLE_OPACITY_MICROMAPS", "0" });
    macros.push_back({ "CAUSTICA_USE_APPROXIMATE_MIS", "1" });
    macros.push_back({ "CAUSTICA_NEE_FULL_SAMPLE_COUNT", "1" });
    macros.push_back({ "CAUSTICA_NEE_LOCAL_CANDIDATE_SAMPLE_COUNT", "3" });
    macros.push_back({ "CAUSTICA_NEE_GLOBAL_CANDIDATE_SAMPLE_COUNT", "2" });
    macros.push_back({ "CAUSTICA_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT", "5" });
    macros.push_back({ "CAUSTICA_DISABLE_SER_TERMINATION_HINT", "0" });
    macros.push_back({ "CAUSTICA_DISCARD_NON_NEE_LIGHTING", "0" });
    macros.push_back({ "CAUSTICA_DISCARD_NEE_LIGHTING", "0" });
    macros.push_back({ "CAUSTICA_FIREFLY_FILTER", "1" });
    macros.push_back({ "CAUSTICA_ACTIVE_STABLE_PLANE_COUNT", "3" });
    macros.push_back({ "CAUSTICA_NESTED_DIELECTRICS_QUALITY", "1" });
    macros.push_back({ "CAUSTICA_LP_TYPES_USE_16BIT_PRECISION", "1" });
    macros.push_back({ "CAUSTICA_ENABLE_LOW_DISCREPANCY_SAMPLER_FOR_BSDF", "1" });
    macros.push_back({ "NEE_AT_SAMPLE_BAKED_ENVIRONMENT", "0" });
}

void setMacro(std::vector<caustica::ShaderMacro>& macros, const char* name, const char* definition)
{
    for (auto& macro : macros)
    {
        if (macro.name == name)
        {
            macro.definition = definition;
            return;
        }
    }
    assert(false);
}

[[nodiscard]] const std::string* findMacroDefinition(
    const std::vector<caustica::ShaderMacro>& macros,
    const char* name)
{
    for (const auto& macro : macros)
    {
        if (macro.name == name)
            return &macro.definition;
    }
    return nullptr;
}

void applyNeeSampleMacros(std::vector<caustica::ShaderMacro>& macros, const PathTracerSettings& s)
{
    const uint localCandidates = ComputeCandidateSampleLocalCount(
        s.ActualNEEAT_LocalToGlobalSampleRatio(),
        (uint)s.NEECandidateSamples);
    const uint globalCandidates = ComputeCandidateSampleGlobalCount(
        s.ActualNEEAT_LocalToGlobalSampleRatio(),
        (uint)s.NEECandidateSamples);
    setMacro(macros, "CAUSTICA_NEE_FULL_SAMPLE_COUNT", std::to_string(s.NEEFullSamples).c_str());
    setMacro(macros, "CAUSTICA_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT", std::to_string(s.NEECandidateSamples).c_str());
    setMacro(macros, "CAUSTICA_NEE_LOCAL_CANDIDATE_SAMPLE_COUNT", std::to_string(localCandidates).c_str());
    setMacro(macros, "CAUSTICA_NEE_GLOBAL_CANDIDATE_SAMPLE_COUNT", std::to_string(globalCandidates).c_str());
}

struct MacroWeight
{
    const char* name;
    int weight;
};

// Higher weight = more important to match. Prefer curated combos over dropping OMM/ReSTIR.
constexpr MacroWeight kResolveWeights[] = {
    { "PT_USE_RESTIR_DI", 100 },
    { "PT_USE_RESTIR_GI", 100 },
    { "PT_USE_RESTIR_PT", 100 },
    { "CAUSTICA_ENABLE_OPACITY_MICROMAPS", 60 },
    { "PT_NEE_ENABLED", 50 },
    { "NEE_AT_SAMPLE_BAKED_ENVIRONMENT", 50 },
    { "CAUSTICA_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT", 25 },
    { "CAUSTICA_NEE_LOCAL_CANDIDATE_SAMPLE_COUNT", 10 },
    { "CAUSTICA_NEE_GLOBAL_CANDIDATE_SAMPLE_COUNT", 10 },
    { "CAUSTICA_ACTIVE_STABLE_PLANE_COUNT", 25 },
    { "CAUSTICA_NESTED_DIELECTRICS_QUALITY", 20 },
    { "PT_ENABLE_RUSSIAN_ROULETTE", 15 },
    { "CAUSTICA_FIREFLY_FILTER", 15 },
    { "CAUSTICA_USE_APPROXIMATE_MIS", 15 },
    { "CAUSTICA_LP_TYPES_USE_16BIT_PRECISION", 15 },
    { "CAUSTICA_ENABLE_LOW_DISCREPANCY_SAMPLER_FOR_BSDF", 15 },
};

[[nodiscard]] int weightedMacroDistance(
    const std::vector<caustica::ShaderMacro>& desired,
    const std::vector<caustica::ShaderMacro>& cooked)
{
    int distance = 0;
    for (const MacroWeight& entry : kResolveWeights)
    {
        const std::string* a = findMacroDefinition(desired, entry.name);
        const std::string* b = findMacroDefinition(cooked, entry.name);
        if (!a || !b || *a != *b)
            distance += entry.weight;
    }
    return distance;
}

void applyPresetOverrides(PtFeaturePresetId id, std::vector<caustica::ShaderMacro>& macros)
{
    switch (id)
    {
    case PtFeaturePresetId::Default:
        break;
    case PtFeaturePresetId::ReSTIR_DI:
        setMacro(macros, "PT_USE_RESTIR_DI", "1");
        break;
    case PtFeaturePresetId::ReSTIR_GI:
        setMacro(macros, "PT_USE_RESTIR_GI", "1");
        break;
    case PtFeaturePresetId::ReSTIR_PT:
        setMacro(macros, "PT_USE_RESTIR_PT", "1");
        break;
    case PtFeaturePresetId::OMM_On:
        setMacro(macros, "CAUSTICA_ENABLE_OPACITY_MICROMAPS", "1");
        break;
    case PtFeaturePresetId::NEE_Off:
        setMacro(macros, "PT_NEE_ENABLED", "0");
        break;
    case PtFeaturePresetId::RR_Off:
        setMacro(macros, "PT_ENABLE_RUSSIAN_ROULETTE", "0");
        break;
    case PtFeaturePresetId::Fp32Types:
        setMacro(macros, "CAUSTICA_LP_TYPES_USE_16BIT_PRECISION", "0");
        break;
    case PtFeaturePresetId::LD_Off:
        setMacro(macros, "CAUSTICA_ENABLE_LOW_DISCREPANCY_SAMPLER_FOR_BSDF", "0");
        break;
    case PtFeaturePresetId::Firefly_Off:
        setMacro(macros, "CAUSTICA_FIREFLY_FILTER", "0");
        break;
    case PtFeaturePresetId::ApproxMIS_Off:
        setMacro(macros, "CAUSTICA_USE_APPROXIMATE_MIS", "0");
        break;
    case PtFeaturePresetId::BakedEnv_On:
        setMacro(macros, "NEE_AT_SAMPLE_BAKED_ENVIRONMENT", "1");
        break;
    case PtFeaturePresetId::NEE_Off_BakedEnv:
        setMacro(macros, "PT_NEE_ENABLED", "0");
        setMacro(macros, "NEE_AT_SAMPLE_BAKED_ENVIRONMENT", "1");
        break;
    case PtFeaturePresetId::NEE_Candidates_8:
        setMacro(macros, "CAUSTICA_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT", "8");
        setMacro(macros, "CAUSTICA_NEE_LOCAL_CANDIDATE_SAMPLE_COUNT", "5");
        setMacro(macros, "CAUSTICA_NEE_GLOBAL_CANDIDATE_SAMPLE_COUNT", "3");
        break;
    case PtFeaturePresetId::StablePlanes_1:
        setMacro(macros, "CAUSTICA_ACTIVE_STABLE_PLANE_COUNT", "1");
        break;
    case PtFeaturePresetId::NestedQuality_2:
        setMacro(macros, "CAUSTICA_NESTED_DIELECTRICS_QUALITY", "2");
        break;

    case PtFeaturePresetId::ReSTIR_DI_OMM:
        setMacro(macros, "PT_USE_RESTIR_DI", "1");
        setMacro(macros, "CAUSTICA_ENABLE_OPACITY_MICROMAPS", "1");
        break;
    case PtFeaturePresetId::ReSTIR_GI_OMM:
        setMacro(macros, "PT_USE_RESTIR_GI", "1");
        setMacro(macros, "CAUSTICA_ENABLE_OPACITY_MICROMAPS", "1");
        break;
    case PtFeaturePresetId::ReSTIR_PT_OMM:
        setMacro(macros, "PT_USE_RESTIR_PT", "1");
        setMacro(macros, "CAUSTICA_ENABLE_OPACITY_MICROMAPS", "1");
        break;
    case PtFeaturePresetId::ReSTIR_DI_BakedEnv:
        setMacro(macros, "PT_USE_RESTIR_DI", "1");
        setMacro(macros, "NEE_AT_SAMPLE_BAKED_ENVIRONMENT", "1");
        break;
    case PtFeaturePresetId::ReSTIR_GI_BakedEnv:
        setMacro(macros, "PT_USE_RESTIR_GI", "1");
        setMacro(macros, "NEE_AT_SAMPLE_BAKED_ENVIRONMENT", "1");
        break;
    case PtFeaturePresetId::ReSTIR_PT_BakedEnv:
        setMacro(macros, "PT_USE_RESTIR_PT", "1");
        setMacro(macros, "NEE_AT_SAMPLE_BAKED_ENVIRONMENT", "1");
        break;
    case PtFeaturePresetId::ReSTIR_DI_OMM_BakedEnv:
        setMacro(macros, "PT_USE_RESTIR_DI", "1");
        setMacro(macros, "CAUSTICA_ENABLE_OPACITY_MICROMAPS", "1");
        setMacro(macros, "NEE_AT_SAMPLE_BAKED_ENVIRONMENT", "1");
        break;
    case PtFeaturePresetId::OMM_BakedEnv:
        setMacro(macros, "CAUSTICA_ENABLE_OPACITY_MICROMAPS", "1");
        setMacro(macros, "NEE_AT_SAMPLE_BAKED_ENVIRONMENT", "1");
        break;
    case PtFeaturePresetId::ReSTIR_DI_NEE8:
        setMacro(macros, "PT_USE_RESTIR_DI", "1");
        setMacro(macros, "CAUSTICA_NEE_TOTAL_CANDIDATE_SAMPLE_COUNT", "8");
        setMacro(macros, "CAUSTICA_NEE_LOCAL_CANDIDATE_SAMPLE_COUNT", "5");
        setMacro(macros, "CAUSTICA_NEE_GLOBAL_CANDIDATE_SAMPLE_COUNT", "3");
        break;
    case PtFeaturePresetId::ReSTIR_DI_StablePlanes_1:
        setMacro(macros, "PT_USE_RESTIR_DI", "1");
        setMacro(macros, "CAUSTICA_ACTIVE_STABLE_PLANE_COUNT", "1");
        break;

    case PtFeaturePresetId::Count:
        assert(false);
        break;
    }
}

} // namespace

std::string_view ptFeaturePresetName(PtFeaturePresetId id)
{
    const uint32_t index = static_cast<uint32_t>(id);
    if (index >= ptFeaturePresetCount())
        return "Invalid";
    return kPresetNames[index];
}

void fillPtFeaturePresetMacros(PtFeaturePresetId id, std::vector<caustica::ShaderMacro>& macros)
{
    fillBaseMacros(macros);
    applyPresetOverrides(id, macros);
}

void fillDesiredPtFeatureMacros(
    const PtFeaturePresetResolveInput& input,
    std::vector<caustica::ShaderMacro>& macros)
{
    assert(input.settings);
    const PathTracerSettings& s = *input.settings;

    fillBaseMacros(macros);

    // Keep NVAPI/DX HitObject / debug viz at cooked defaults (0). Those stay out of the matrix.
    setMacro(macros, "PT_ENABLE_RUSSIAN_ROULETTE", s.EnableRussianRoulette ? "1" : "0");
    setMacro(macros, "PT_NEE_ENABLED", s.UseNEE ? "1" : "0");
    setMacro(macros, "PT_USE_RESTIR_DI", s.actualUseReSTIRDI() ? "1" : "0");
    setMacro(macros, "PT_USE_RESTIR_GI", s.actualUseReSTIRGI() ? "1" : "0");
    setMacro(macros, "PT_USE_RESTIR_PT", s.actualUseReSTIRPT() ? "1" : "0");
    setMacro(macros, "CAUSTICA_ENABLE_OPACITY_MICROMAPS", input.useOpacityMicromaps ? "1" : "0");
    setMacro(macros, "CAUSTICA_USE_APPROXIMATE_MIS", s.actualUseApproximateMIS() ? "1" : "0");

    applyNeeSampleMacros(macros, s);

    setMacro(macros, "CAUSTICA_FIREFLY_FILTER", s.actualFireflyFilterEnabled() ? "1" : "0");
    setMacro(macros, "CAUSTICA_ACTIVE_STABLE_PLANE_COUNT", std::to_string(s.StablePlanesActiveCount).c_str());
    setMacro(macros, "CAUSTICA_NESTED_DIELECTRICS_QUALITY", std::to_string(s.NestedDielectricsQuality).c_str());
    setMacro(macros, "CAUSTICA_LP_TYPES_USE_16BIT_PRECISION", s.UseFp16Types ? "1" : "0");
    setMacro(macros, "CAUSTICA_ENABLE_LOW_DISCREPANCY_SAMPLER_FOR_BSDF", s.EnableLDSamplerForBSDF ? "1" : "0");
    setMacro(macros, "NEE_AT_SAMPLE_BAKED_ENVIRONMENT", input.sampleBakedEnvironment ? "1" : "0");
}

PtFeaturePresetId resolvePtFeaturePreset(const PtFeaturePresetResolveInput& input)
{
    std::vector<caustica::ShaderMacro> desired;
    fillDesiredPtFeatureMacros(input, desired);

    PtFeaturePresetId best = PtFeaturePresetId::Default;
    int bestDistance = std::numeric_limits<int>::max();

    std::vector<caustica::ShaderMacro> cooked;
    for (uint32_t i = 0; i < ptFeaturePresetCount(); ++i)
    {
        const auto id = static_cast<PtFeaturePresetId>(i);
        fillPtFeaturePresetMacros(id, cooked);
        const int distance = weightedMacroDistance(desired, cooked);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = id;
            if (distance == 0)
                break;
        }
    }

    return best;
}

} // namespace caustica::render

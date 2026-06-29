#include <render/RenderSessionState.h>

#include <core/command_line.h>
#include <render/Passes/Denoisers/NrdConfig.h>

#if CAUSTICA_WITH_ANY_DLSS
#include <backend/StreamlineInterface.h>
using SI = caustica::StreamlineInterface;
#endif

namespace caustica::render
{

namespace
{

const PerformancePreset kDefaultPerformancePreset{
    "Balanced",
    5,  // NEECandidateSamples
    1,  // NEEFullSamples
    1,  // NEEMISType
    1,  // RealtimeSamplesPerPixel
    18, // BounceCount
    2,  // DiffuseBounceCount
    -1.0f, // TexLODBias
    1,  // NestedDielectricsQuality
    2,  // EnvironmentMapDiffuseSampleMIPLevel
    3,  // StablePlanesActiveCount
    true,  // AllowPrimarySurfaceReplacement
    true,  // EnableBloom
    true,  // EnableLDSamplerForBSDF
    0.1f, // FireflyThreshold
#if CAUSTICA_WITH_ANY_DLSS
    SI::DLSSMode::eBalanced,
#endif
};

} // namespace

void ApplyPerformancePreset(PathTracerSettings& settings, const PerformancePreset& preset)
{
    settings.NEECandidateSamples = preset.NEECandidateSamples;
    settings.NEEFullSamples = preset.NEEFullSamples;
    settings.NEEMISType = preset.NEEMISType;
    settings.RealtimeSamplesPerPixel = preset.RealtimeSamplesPerPixel;
    settings.BounceCount = preset.BounceCount;
    settings.DiffuseBounceCount = preset.DiffuseBounceCount;
    settings.TexLODBias = preset.TexLODBias;
    settings.NestedDielectricsQuality = preset.NestedDielectricsQuality;
    settings.EnvironmentMapDiffuseSampleMIPLevel = preset.EnvironmentMapDiffuseSampleMIPLevel;
#if CAUSTICA_WITH_ANY_DLSS
    settings.DLSSMode = preset.DLSSMode;
#endif
    settings.StablePlanesActiveCount = preset.StablePlanesActiveCount;
    settings.AllowPrimarySurfaceReplacement = preset.AllowPrimarySurfaceReplacement;
    settings.EnableBloom = preset.EnableBloom;
    settings.EnableLDSamplerForBSDF = preset.EnableLDSamplerForBSDF;
    settings.RealtimeFireflyFilterThreshold = preset.FireflyThreshold;
    settings.ResetAccumulation = true;
}

bool MatchesPerformancePreset(const PathTracerSettings& settings, const PerformancePreset& preset)
{
    if (settings.NEECandidateSamples != preset.NEECandidateSamples) return false;
    if (settings.NEEFullSamples != preset.NEEFullSamples) return false;
    if (settings.NEEMISType != preset.NEEMISType) return false;
    if (settings.RealtimeSamplesPerPixel != preset.RealtimeSamplesPerPixel) return false;
    if (settings.BounceCount != preset.BounceCount) return false;
    if (settings.DiffuseBounceCount != preset.DiffuseBounceCount) return false;
    if (settings.TexLODBias != preset.TexLODBias) return false;
    if (settings.NestedDielectricsQuality != preset.NestedDielectricsQuality) return false;
    if (settings.EnvironmentMapDiffuseSampleMIPLevel != preset.EnvironmentMapDiffuseSampleMIPLevel) return false;
#if CAUSTICA_WITH_ANY_DLSS
    if (settings.DLSSMode != preset.DLSSMode) return false;
#endif
    if (settings.StablePlanesActiveCount != preset.StablePlanesActiveCount) return false;
    if (settings.AllowPrimarySurfaceReplacement != preset.AllowPrimarySurfaceReplacement) return false;
    if (settings.EnableBloom != preset.EnableBloom) return false;
    if (settings.EnableLDSamplerForBSDF != preset.EnableLDSamplerForBSDF) return false;
    if (settings.RealtimeFireflyFilterThreshold != preset.FireflyThreshold) return false;
    return true;
}

void InitializeRenderSessionStateFromCommandLine(RenderSessionState& state, const CommandLineOptions& cmdLine)
{
    state.RelaxSettings = NrdConfig::getDefaultRELAXSettings();
    state.ReblurSettings = NrdConfig::getDefaultREBLURSettings();

    state.TemporalAntiAliasingParams.useHistoryClampRelax = true;
    state.ToneMappingParams.toneMapOperator = ToneMapperOperator::HableUc2;

    state.RTXDI.regir.regirStaticParams.Mode = rtxdi::ReGIRMode::Grid;

    state.UseNEE = cmdLine.UseNEE != 0;
    state.NEEType = cmdLine.NEEType;
    state.UseReSTIRDI = cmdLine.UseReSTIRDI != 0;
    state.UseReSTIRGI = cmdLine.UseReSTIRGI != 0;
    state.UseReSTIRPT = cmdLine.UseReSTIRPT != 0;
    if (state.UseReSTIRPT)
        state.UseReSTIRGI = false;
    state.RealtimeSamplesPerPixel = cmdLine.RealtimeSamplesPerPixel;
    state.AccumulationTarget = cmdLine.ReferenceSamplesPerPixel;
    state.StandaloneDenoiser = cmdLine.StandaloneDenoiser != 0;
    state.RealtimeAA = cmdLine.RealtimeAA;

    ApplyPerformancePreset(state, kDefaultPerformancePreset);
    state.RTXDIRestirPreset = RTXDIRestirQualityPreset::Ultra;
    state.ApplyRTXDIRestirPreset();
    state.RTXDIRestirPTPreset = RTXDIRestirPTQualityPreset::Ultra;
    state.ApplyRTXDIRestirPTPreset();

    state.EnableBloom &= !cmdLine.DisablePostProcessFilters;
}

} // namespace caustica::render

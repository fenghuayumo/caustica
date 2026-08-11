#pragma once

#include <render/passes/rtxdi/RtxdiApplicationSettings.h>

#include <rtxdi/ImportanceSamplingContext.h>
#include <rtxdi/PT/ReSTIRPT.h>

namespace caustica::render::rtxdi_sdk
{
inline ::rtxdi::CheckerboardMode toSdk(caustica::rtxdi_config::CheckerboardMode value)
{
    return static_cast<::rtxdi::CheckerboardMode>(value);
}

inline ::rtxdi::ReSTIRDI_ResamplingMode toSdk(caustica::rtxdi_config::DIResamplingMode value)
{
    return static_cast<::rtxdi::ReSTIRDI_ResamplingMode>(value);
}

inline ::rtxdi::ReSTIRGI_ResamplingMode toSdk(caustica::rtxdi_config::GIResamplingMode value)
{
    return static_cast<::rtxdi::ReSTIRGI_ResamplingMode>(value);
}

inline ::rtxdi::ReSTIRPT_ResamplingMode toSdk(caustica::rtxdi_config::PTResamplingMode value)
{
    return static_cast<::rtxdi::ReSTIRPT_ResamplingMode>(value);
}

inline ReSTIRDI_InitialSamplingParameters toSdk(const caustica::rtxdi_config::DIInitialSamplingParameters& value)
{
    ReSTIRDI_InitialSamplingParameters result{};
    result.numPrimaryLocalLightSamples = value.numPrimaryLocalLightSamples;
    result.numPrimaryInfiniteLightSamples = value.numPrimaryInfiniteLightSamples;
    result.numPrimaryEnvironmentSamples = value.numPrimaryEnvironmentSamples;
    result.numPrimaryBrdfSamples = value.numPrimaryBrdfSamples;
    result.brdfCutoff = value.brdfCutoff;
    result.enableInitialVisibility = value.enableInitialVisibility;
    result.environmentMapImportanceSampling = value.environmentMapImportanceSampling;
    result.localLightSamplingMode = static_cast<ReSTIRDI_LocalLightSamplingMode>(value.localLightSamplingMode);
    return result;
}

inline ReSTIRDI_TemporalResamplingParameters toSdk(const caustica::rtxdi_config::DITemporalResamplingParameters& value)
{
    ReSTIRDI_TemporalResamplingParameters result{};
    result.temporalDepthThreshold = value.temporalDepthThreshold;
    result.temporalNormalThreshold = value.temporalNormalThreshold;
    result.maxHistoryLength = value.maxHistoryLength;
    result.temporalBiasCorrection = static_cast<ReSTIRDI_TemporalBiasCorrectionMode>(value.temporalBiasCorrection);
    result.enablePermutationSampling = value.enablePermutationSampling;
    result.permutationSamplingThreshold = value.permutationSamplingThreshold;
    result.enableBoilingFilter = value.enableBoilingFilter;
    result.boilingFilterStrength = value.boilingFilterStrength;
    result.discardInvisibleSamples = value.discardInvisibleSamples;
    return result;
}

inline ReSTIRDI_SpatialResamplingParameters toSdk(const caustica::rtxdi_config::DISpatialResamplingParameters& value)
{
    ReSTIRDI_SpatialResamplingParameters result{};
    result.spatialDepthThreshold = value.spatialDepthThreshold;
    result.spatialNormalThreshold = value.spatialNormalThreshold;
    result.spatialBiasCorrection = static_cast<ReSTIRDI_SpatialBiasCorrectionMode>(value.spatialBiasCorrection);
    result.numSpatialSamples = value.numSpatialSamples;
    result.numDisocclusionBoostSamples = value.numDisocclusionBoostSamples;
    result.spatialSamplingRadius = value.spatialSamplingRadius;
    result.discountNaiveSamples = value.discountNaiveSamples;
    return result;
}

inline ReSTIRDI_ShadingParameters toSdk(const caustica::rtxdi_config::DIShadingParameters& value)
{
    ReSTIRDI_ShadingParameters result{};
    result.enableFinalVisibility = value.enableFinalVisibility;
    result.reuseFinalVisibility = value.reuseFinalVisibility;
    result.finalVisibilityMaxAge = value.finalVisibilityMaxAge;
    result.finalVisibilityMaxDistance = value.finalVisibilityMaxDistance;
    result.enableDenoiserInputPacking = value.enableDenoiserInputPacking;
    return result;
}

inline ReSTIRGI_TemporalResamplingParameters toSdk(const caustica::rtxdi_config::GITemporalResamplingParameters& value)
{
    ReSTIRGI_TemporalResamplingParameters result{};
    result.depthThreshold = value.depthThreshold;
    result.normalThreshold = value.normalThreshold;
    result.enablePermutationSampling = value.enablePermutationSampling;
    result.maxHistoryLength = value.maxHistoryLength;
    result.maxReservoirAge = value.maxReservoirAge;
    result.enableBoilingFilter = value.enableBoilingFilter;
    result.boilingFilterStrength = value.boilingFilterStrength;
    result.enableFallbackSampling = value.enableFallbackSampling;
    result.temporalBiasCorrectionMode = static_cast<ResTIRGI_TemporalBiasCorrectionMode>(value.temporalBiasCorrectionMode);
    return result;
}

inline ReSTIRGI_SpatialResamplingParameters toSdk(const caustica::rtxdi_config::GISpatialResamplingParameters& value)
{
    ReSTIRGI_SpatialResamplingParameters result{};
    result.spatialDepthThreshold = value.spatialDepthThreshold;
    result.spatialNormalThreshold = value.spatialNormalThreshold;
    result.numSpatialSamples = value.numSpatialSamples;
    result.spatialSamplingRadius = value.spatialSamplingRadius;
    result.spatialBiasCorrectionMode = static_cast<ResTIRGI_SpatialBiasCorrectionMode>(value.spatialBiasCorrectionMode);
    return result;
}

inline ReSTIRGI_FinalShadingParameters toSdk(const caustica::rtxdi_config::GIFinalShadingParameters& value)
{
    ReSTIRGI_FinalShadingParameters result{};
    result.enableFinalVisibility = value.enableFinalVisibility;
    result.enableFinalMIS = value.enableFinalMIS;
    return result;
}

inline RTXDI_PTInitialSamplingParameters toSdk(const caustica::rtxdi_config::PTInitialSamplingParameters& value)
{
    RTXDI_PTInitialSamplingParameters result{};
    result.numInitialSamples = value.numInitialSamples;
    result.maxBounceDepth = value.maxBounceDepth;
    result.maxRcVertexLength = value.maxRcVertexLength;
    return result;
}

inline RTXDI_PTTemporalResamplingParameters toSdk(const caustica::rtxdi_config::PTTemporalResamplingParameters& value)
{
    RTXDI_PTTemporalResamplingParameters result{};
    result.depthThreshold = value.depthThreshold;
    result.normalThreshold = value.normalThreshold;
    result.enablePermutationSampling = value.enablePermutationSampling;
    result.maxHistoryLength = value.maxHistoryLength;
    result.maxReservoirAge = value.maxReservoirAge;
    result.enableFallbackSampling = value.enableFallbackSampling;
    result.enableVisibilityBeforeCombine = value.enableVisibilityBeforeCombine;
    result.duplicationBasedHistoryReduction = value.duplicationBasedHistoryReduction;
    result.historyReductionStrength = value.historyReductionStrength;
    return result;
}

inline RTXDI_PTReconnectionParameters toSdk(const caustica::rtxdi_config::PTReconnectionParameters& value)
{
    RTXDI_PTReconnectionParameters result{};
    result.minConnectionFootprint = value.minConnectionFootprint;
    result.minConnectionFootprintSigma = value.minConnectionFootprintSigma;
    result.minPdfRoughness = value.minPdfRoughness;
    result.minPdfRoughnessSigma = value.minPdfRoughnessSigma;
    result.roughnessThreshold = value.roughnessThreshold;
    result.distanceThreshold = value.distanceThreshold;
    result.reconnectionMode = static_cast<RTXDI_PTReconnectionMode>(value.reconnectionMode);
    return result;
}

inline RTXDI_PTHybridShiftPerFrameParameters toSdk(const caustica::rtxdi_config::PTHybridShiftParameters& value)
{
    RTXDI_PTHybridShiftPerFrameParameters result{};
    result.maxBounceDepth = value.maxBounceDepth;
    result.maxRcVertexLength = value.maxRcVertexLength;
    return result;
}

inline RTXDI_BoilingFilterParameters toSdk(const caustica::rtxdi_config::BoilingFilterParameters& value)
{
    RTXDI_BoilingFilterParameters result{};
    result.enableBoilingFilter = value.enableBoilingFilter;
    result.boilingFilterStrength = value.boilingFilterStrength;
    return result;
}

inline RTXDI_PTSpatialResamplingParameters toSdk(const caustica::rtxdi_config::PTSpatialResamplingParameters& value)
{
    RTXDI_PTSpatialResamplingParameters result{};
    result.numSpatialSamples = value.numSpatialSamples;
    result.numDisocclusionBoostSamples = value.numDisocclusionBoostSamples;
    result.maxTemporalHistory = value.maxTemporalHistory;
    result.duplicationBasedHistoryReduction = value.duplicationBasedHistoryReduction;
    result.samplingRadius = value.samplingRadius;
    result.normalThreshold = value.normalThreshold;
    result.depthThreshold = value.depthThreshold;
    return result;
}

inline ::rtxdi::ReGIRStaticParameters toSdk(const caustica::rtxdi_config::ReGIRStaticParameters& value)
{
    ::rtxdi::ReGIRStaticParameters result{};
    result.Mode = static_cast<::rtxdi::ReGIRMode>(value.Mode);
    result.LightsPerCell = value.LightsPerCell;
    result.gridParameters.GridSize = { value.GridSize.x, value.GridSize.y, value.GridSize.z };
    result.onionParameters.OnionDetailLayers = value.OnionDetailLayers;
    result.onionParameters.OnionCoverageLayers = value.OnionCoverageLayers;
    return result;
}

inline ::rtxdi::ReGIRDynamicParameters toSdk(const caustica::rtxdi_config::ReGIRDynamicParameters& value)
{
    ::rtxdi::ReGIRDynamicParameters result{};
    result.regirCellSize = value.regirCellSize;
    result.center = { value.center.x, value.center.y, value.center.z };
    result.fallbackSamplingMode = static_cast<::rtxdi::LocalLightReGIRFallbackSamplingMode>(value.fallbackSamplingMode);
    result.presamplingMode = static_cast<::rtxdi::LocalLightReGIRPresamplingMode>(value.presamplingMode);
    result.regirSamplingJitter = value.regirSamplingJitter;
    result.regirNumBuildSamples = value.regirNumBuildSamples;
    return result;
}
} // namespace caustica::render::rtxdi_sdk

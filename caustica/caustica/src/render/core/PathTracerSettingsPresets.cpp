#include <render/core/PathTracerSettings.h>
#include <render/passes/rtxdi/RtxdiApplicationSettings.h>

void PathTracerSettings::applyRTXDIRestirPreset()
{
    if (RTXDIRestirPreset == RTXDIRestirQualityPreset::Custom)
        return;

    const bool wasUsingCheckerboard = RTXDI.checkerboardMode != rtxdi::CheckerboardMode::Off;
    bool enableCheckerboardSampling = wasUsingCheckerboard;

    RTXDI.restirDI.resamplingMode = getReSTIRDI_ResamplingMode();
    RTXDI.restirDI.initialSamplingParams = getReSTIRDIInitialSamplingParams();
    RTXDI.restirDI.temporalResamplingParams = getReSTIRDITemporalResamplingParams();
    RTXDI.restirDI.spatialResamplingParams = getReSTIRDISpatialResamplingParams();
    RTXDI.restirDI.shadingParams = getReSTIRDIShadingParams();

    RTXDI.restirGI.resamplingMode = getReSTIRGI_ResamplingMode();
    RTXDI.restirGI.temporalResamplingParams = getReSTIRGITemporalResamplingParams();
    RTXDI.restirGI.spatialResamplingParams = getReSTIRGISpatialResamplingParams();
    RTXDI.restirGI.finalShadingParams = getReSTIRGIFinalShadingParams();

    switch (RTXDIRestirPreset)
    {
    case RTXDIRestirQualityPreset::Fast:
        enableCheckerboardSampling = true;
        RTXDI.restirDI.resamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Power_RIS;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 4;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 0;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.discardInvisibleSamples = true;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = true;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.2f;
        RTXDI.restirDI.temporalResamplingParams.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Off;
        RTXDI.restirDI.spatialResamplingParams.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Off;
        RTXDI.restirDI.spatialResamplingParams.numSpatialSamples = 1;
        RTXDI.restirDI.spatialResamplingParams.numDisocclusionBoostSamples = 2;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = true;

        RTXDI.restirGI.resamplingMode = rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirGI.temporalResamplingParams.maxHistoryLength = 6;
        RTXDI.restirGI.temporalResamplingParams.maxReservoirAge = 30;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = true;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.35f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = ResTIRGI_TemporalBiasCorrectionMode::Basic;
        RTXDI.restirGI.spatialResamplingParams.numSpatialSamples = 1;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = ResTIRGI_SpatialBiasCorrectionMode::Basic;
        break;

    case RTXDIRestirQualityPreset::Medium:
        enableCheckerboardSampling = false;
        RTXDI.restirDI.resamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 8;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.discardInvisibleSamples = true;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = true;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.2f;
        RTXDI.restirDI.temporalResamplingParams.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Basic;
        RTXDI.restirDI.spatialResamplingParams.numSpatialSamples = 1;
        RTXDI.restirDI.spatialResamplingParams.numDisocclusionBoostSamples = 8;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = true;

        RTXDI.restirGI.resamplingMode = rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirGI.temporalResamplingParams.maxHistoryLength = 10;
        RTXDI.restirGI.temporalResamplingParams.maxReservoirAge = 50;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = true;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.35f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = ResTIRGI_TemporalBiasCorrectionMode::Basic;
        RTXDI.restirGI.spatialResamplingParams.numSpatialSamples = 2;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = ResTIRGI_SpatialBiasCorrectionMode::Basic;
        break;

    case RTXDIRestirQualityPreset::Unbiased:
        enableCheckerboardSampling = false;
        RTXDI.restirDI.resamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Uniform;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 8;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.discardInvisibleSamples = false;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirDI.temporalResamplingParams.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.numSpatialSamples = 1;
        RTXDI.restirDI.spatialResamplingParams.numDisocclusionBoostSamples = 8;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = false;

        RTXDI.restirGI.resamplingMode = rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = ResTIRGI_TemporalBiasCorrectionMode::Raytraced;
        RTXDI.restirGI.spatialResamplingParams.numSpatialSamples = 2;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = ResTIRGI_SpatialBiasCorrectionMode::Raytraced;
        break;

    case RTXDIRestirQualityPreset::Ultra:
        enableCheckerboardSampling = false;
        RTXDI.restirDI.resamplingMode = rtxdi::ReSTIRDI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::ReGIR_RIS;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 16;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.discardInvisibleSamples = false;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirDI.temporalResamplingParams.temporalBiasCorrection = ReSTIRDI_TemporalBiasCorrectionMode::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.spatialBiasCorrection = ReSTIRDI_SpatialBiasCorrectionMode::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.numSpatialSamples = 4;
        RTXDI.restirDI.spatialResamplingParams.numDisocclusionBoostSamples = 16;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = false;

        RTXDI.restirGI.resamplingMode = rtxdi::ReSTIRGI_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirGI.temporalResamplingParams.maxHistoryLength = 20;
        RTXDI.restirGI.temporalResamplingParams.maxReservoirAge = 50;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = ResTIRGI_TemporalBiasCorrectionMode::Raytraced;
        RTXDI.restirGI.spatialResamplingParams.numSpatialSamples = 4;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = ResTIRGI_SpatialBiasCorrectionMode::Raytraced;
        break;

    case RTXDIRestirQualityPreset::Reference:
        enableCheckerboardSampling = false;
        RTXDI.restirDI.resamplingMode = rtxdi::ReSTIRDI_ResamplingMode::None;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode::Uniform;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 16;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = false;

        RTXDI.restirGI.resamplingMode = rtxdi::ReSTIRGI_ResamplingMode::None;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = ResTIRGI_TemporalBiasCorrectionMode::Raytraced;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = ResTIRGI_SpatialBiasCorrectionMode::Raytraced;
        break;

    case RTXDIRestirQualityPreset::Custom:
    default:
        break;
    }

    RTXDI.checkerboardMode = enableCheckerboardSampling ? rtxdi::CheckerboardMode::Black : rtxdi::CheckerboardMode::Off;
    ResetAccumulation = true;
    ResetRealtimeCaches |= wasUsingCheckerboard != enableCheckerboardSampling;
}

void PathTracerSettings::applyRTXDIRestirPTPreset()
{
    if (RTXDIRestirPTPreset == RTXDIRestirPTQualityPreset::Custom)
        return;

    RTXDI.restirPT.initialSamplingParams = getReSTIRPTInitialSamplingParams();
    RTXDI.restirPT.temporalResamplingParams = getReSTIRPTTemporalResamplingParams();
    RTXDI.restirPT.reconnectionParams = getReSTIRPTReconnectionParams();
    RTXDI.restirPT.hybridShiftParams = getReSTIRPTHybridShiftParams();
    RTXDI.restirPT.boilingFilterParams = getReSTIRPTBoilingFilterParams();
    RTXDI.restirPT.spatialResamplingParams = getReSTIRPTSpatialResamplingParams();

    switch (RTXDIRestirPTPreset)
    {
    case RTXDIRestirPTQualityPreset::Fast:
        RTXDI.restirPT.resamplingMode = rtxdi::ReSTIRPT_ResamplingMode::Temporal;
        RTXDI.restirPT.initialSamplingParams.maxBounceDepth = 3;
        RTXDI.restirPT.initialSamplingParams.maxRcVertexLength = RTXDI.restirPT.initialSamplingParams.maxBounceDepth + 1;
        RTXDI.restirPT.initialSamplingParams.numInitialSamples = 1;
        RTXDI.restirPT.spatialResamplingParams.numDisocclusionBoostSamples = 2;
        RTXDI.restirPT.spatialResamplingParams.samplingRadius = 32.0f;
        RTXDI.restirPT.spatialResamplingParams.numSpatialSamples = 1;
        break;

    case RTXDIRestirPTQualityPreset::Medium:
        RTXDI.restirPT.resamplingMode = rtxdi::ReSTIRPT_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirPT.initialSamplingParams.maxBounceDepth = 3;
        RTXDI.restirPT.initialSamplingParams.maxRcVertexLength = RTXDI.restirPT.initialSamplingParams.maxBounceDepth + 1;
        RTXDI.restirPT.initialSamplingParams.numInitialSamples = 1;
        RTXDI.restirPT.spatialResamplingParams.numDisocclusionBoostSamples = 4;
        RTXDI.restirPT.spatialResamplingParams.samplingRadius = 32.0f;
        RTXDI.restirPT.spatialResamplingParams.numSpatialSamples = 1;
        break;

    case RTXDIRestirPTQualityPreset::Ultra:
        RTXDI.restirPT.resamplingMode = rtxdi::ReSTIRPT_ResamplingMode::TemporalAndSpatial;
        RTXDI.restirPT.initialSamplingParams.maxBounceDepth = 4;
        RTXDI.restirPT.initialSamplingParams.maxRcVertexLength = RTXDI.restirPT.initialSamplingParams.maxBounceDepth + 1;
        RTXDI.restirPT.initialSamplingParams.numInitialSamples = 1;
        RTXDI.restirPT.spatialResamplingParams.numDisocclusionBoostSamples = 8;
        RTXDI.restirPT.spatialResamplingParams.samplingRadius = 32.0f;
        RTXDI.restirPT.spatialResamplingParams.numSpatialSamples = 1;
        break;

    case RTXDIRestirPTQualityPreset::Custom:
    default:
        break;
    }

    RTXDI.restirPT.hybridShiftParams.maxBounceDepth = RTXDI.restirPT.initialSamplingParams.maxBounceDepth;
    RTXDI.restirPT.hybridShiftParams.maxRcVertexLength = RTXDI.restirPT.initialSamplingParams.maxRcVertexLength;
    RTXDI.restirPT.spatialResamplingParams.maxTemporalHistory = RTXDI.restirPT.temporalResamplingParams.maxHistoryLength;
    ResetAccumulation = true;
}

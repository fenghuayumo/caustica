#include <render/core/PathTracerSettings.h>
#include <render/passes/rtxdi/RtxdiApplicationSettings.h>

void PathTracerSettings::applyRTXDIRestirPreset()
{
    if (RTXDIRestirPreset == RTXDIRestirQualityPreset::Custom)
        return;

    const bool wasUsingCheckerboard = RTXDI.checkerboardMode != caustica::rtxdi_config::CheckerboardMode::Off;
    bool enableCheckerboardSampling = wasUsingCheckerboard;

    RTXDI.restirDI.resamplingMode = caustica::rtxdi_config::DIResamplingMode::TemporalAndSpatial;
    RTXDI.restirDI.initialSamplingParams = caustica::rtxdi_config::defaultDIInitialSampling();
    RTXDI.restirDI.temporalResamplingParams = caustica::rtxdi_config::defaultDITemporalResampling();
    RTXDI.restirDI.spatialResamplingParams = caustica::rtxdi_config::defaultDISpatialResampling();
    RTXDI.restirDI.shadingParams = caustica::rtxdi_config::defaultDIShading();

    RTXDI.restirGI.resamplingMode = caustica::rtxdi_config::GIResamplingMode::TemporalAndSpatial;
    RTXDI.restirGI.temporalResamplingParams = caustica::rtxdi_config::defaultGITemporalResampling();
    RTXDI.restirGI.spatialResamplingParams = caustica::rtxdi_config::defaultGISpatialResampling();
    RTXDI.restirGI.finalShadingParams = caustica::rtxdi_config::defaultGIFinalShading();

    switch (RTXDIRestirPreset)
    {
    case RTXDIRestirQualityPreset::Fast:
        enableCheckerboardSampling = true;
        RTXDI.restirDI.resamplingMode = caustica::rtxdi_config::DIResamplingMode::TemporalAndSpatial;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = caustica::rtxdi_config::LocalLightSamplingMode::PowerRIS;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 4;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 0;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.discardInvisibleSamples = true;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = true;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.2f;
        RTXDI.restirDI.temporalResamplingParams.temporalBiasCorrection = caustica::rtxdi_config::DITemporalBiasCorrection::Off;
        RTXDI.restirDI.spatialResamplingParams.spatialBiasCorrection = caustica::rtxdi_config::DISpatialBiasCorrection::Off;
        RTXDI.restirDI.spatialResamplingParams.numSpatialSamples = 1;
        RTXDI.restirDI.spatialResamplingParams.numDisocclusionBoostSamples = 2;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = true;

        RTXDI.restirGI.resamplingMode = caustica::rtxdi_config::GIResamplingMode::TemporalAndSpatial;
        RTXDI.restirGI.temporalResamplingParams.maxHistoryLength = 6;
        RTXDI.restirGI.temporalResamplingParams.maxReservoirAge = 30;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = true;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.35f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = caustica::rtxdi_config::GITemporalBiasCorrection::Basic;
        RTXDI.restirGI.spatialResamplingParams.numSpatialSamples = 1;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = caustica::rtxdi_config::GISpatialBiasCorrection::Basic;
        break;

    case RTXDIRestirQualityPreset::Medium:
        enableCheckerboardSampling = false;
        RTXDI.restirDI.resamplingMode = caustica::rtxdi_config::DIResamplingMode::TemporalAndSpatial;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = caustica::rtxdi_config::LocalLightSamplingMode::ReGIRRIS;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 8;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.discardInvisibleSamples = true;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = true;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.2f;
        RTXDI.restirDI.temporalResamplingParams.temporalBiasCorrection = caustica::rtxdi_config::DITemporalBiasCorrection::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.spatialBiasCorrection = caustica::rtxdi_config::DISpatialBiasCorrection::Basic;
        RTXDI.restirDI.spatialResamplingParams.numSpatialSamples = 1;
        RTXDI.restirDI.spatialResamplingParams.numDisocclusionBoostSamples = 8;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = true;

        RTXDI.restirGI.resamplingMode = caustica::rtxdi_config::GIResamplingMode::TemporalAndSpatial;
        RTXDI.restirGI.temporalResamplingParams.maxHistoryLength = 10;
        RTXDI.restirGI.temporalResamplingParams.maxReservoirAge = 50;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = true;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.35f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = caustica::rtxdi_config::GITemporalBiasCorrection::Basic;
        RTXDI.restirGI.spatialResamplingParams.numSpatialSamples = 2;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = caustica::rtxdi_config::GISpatialBiasCorrection::Basic;
        break;

    case RTXDIRestirQualityPreset::Unbiased:
        enableCheckerboardSampling = false;
        RTXDI.restirDI.resamplingMode = caustica::rtxdi_config::DIResamplingMode::TemporalAndSpatial;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = caustica::rtxdi_config::LocalLightSamplingMode::Uniform;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 8;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.discardInvisibleSamples = false;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirDI.temporalResamplingParams.temporalBiasCorrection = caustica::rtxdi_config::DITemporalBiasCorrection::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.spatialBiasCorrection = caustica::rtxdi_config::DISpatialBiasCorrection::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.numSpatialSamples = 1;
        RTXDI.restirDI.spatialResamplingParams.numDisocclusionBoostSamples = 8;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = false;

        RTXDI.restirGI.resamplingMode = caustica::rtxdi_config::GIResamplingMode::TemporalAndSpatial;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = caustica::rtxdi_config::GITemporalBiasCorrection::Raytraced;
        RTXDI.restirGI.spatialResamplingParams.numSpatialSamples = 2;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = caustica::rtxdi_config::GISpatialBiasCorrection::Raytraced;
        break;

    case RTXDIRestirQualityPreset::Ultra:
        enableCheckerboardSampling = false;
        RTXDI.restirDI.resamplingMode = caustica::rtxdi_config::DIResamplingMode::TemporalAndSpatial;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = caustica::rtxdi_config::LocalLightSamplingMode::ReGIRRIS;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 16;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.discardInvisibleSamples = false;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirDI.temporalResamplingParams.temporalBiasCorrection = caustica::rtxdi_config::DITemporalBiasCorrection::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.spatialBiasCorrection = caustica::rtxdi_config::DISpatialBiasCorrection::Raytraced;
        RTXDI.restirDI.spatialResamplingParams.numSpatialSamples = 4;
        RTXDI.restirDI.spatialResamplingParams.numDisocclusionBoostSamples = 16;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = false;

        RTXDI.restirGI.resamplingMode = caustica::rtxdi_config::GIResamplingMode::TemporalAndSpatial;
        RTXDI.restirGI.temporalResamplingParams.maxHistoryLength = 20;
        RTXDI.restirGI.temporalResamplingParams.maxReservoirAge = 50;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = caustica::rtxdi_config::GITemporalBiasCorrection::Raytraced;
        RTXDI.restirGI.spatialResamplingParams.numSpatialSamples = 4;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = caustica::rtxdi_config::GISpatialBiasCorrection::Raytraced;
        break;

    case RTXDIRestirQualityPreset::Reference:
        enableCheckerboardSampling = false;
        RTXDI.restirDI.resamplingMode = caustica::rtxdi_config::DIResamplingMode::None;
        RTXDI.restirDI.initialSamplingParams.localLightSamplingMode = caustica::rtxdi_config::LocalLightSamplingMode::Uniform;
        RTXDI.restirDI.initialSamplingParams.numPrimaryLocalLightSamples = 16;
        RTXDI.restirDI.initialSamplingParams.numPrimaryBrdfSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryInfiniteLightSamples = 1;
        RTXDI.restirDI.initialSamplingParams.numPrimaryEnvironmentSamples = 1;
        RTXDI.restirDI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirDI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirDI.shadingParams.reuseFinalVisibility = false;

        RTXDI.restirGI.resamplingMode = caustica::rtxdi_config::GIResamplingMode::None;
        RTXDI.restirGI.temporalResamplingParams.enableBoilingFilter = false;
        RTXDI.restirGI.temporalResamplingParams.boilingFilterStrength = 0.0f;
        RTXDI.restirGI.temporalResamplingParams.temporalBiasCorrectionMode = caustica::rtxdi_config::GITemporalBiasCorrection::Raytraced;
        RTXDI.restirGI.spatialResamplingParams.spatialBiasCorrectionMode = caustica::rtxdi_config::GISpatialBiasCorrection::Raytraced;
        break;

    case RTXDIRestirQualityPreset::Custom:
    default:
        break;
    }

    RTXDI.checkerboardMode = enableCheckerboardSampling ? caustica::rtxdi_config::CheckerboardMode::Black : caustica::rtxdi_config::CheckerboardMode::Off;
    ResetAccumulation = true;
    ResetRealtimeCaches |= wasUsingCheckerboard != enableCheckerboardSampling;
}

void PathTracerSettings::applyRTXDIRestirPTPreset()
{
    if (RTXDIRestirPTPreset == RTXDIRestirPTQualityPreset::Custom)
        return;

    RTXDI.restirPT.initialSamplingParams = caustica::rtxdi_config::defaultPTInitialSampling();
    RTXDI.restirPT.temporalResamplingParams = caustica::rtxdi_config::defaultPTTemporalResampling();
    RTXDI.restirPT.reconnectionParams = caustica::rtxdi_config::defaultPTReconnection();
    RTXDI.restirPT.hybridShiftParams = caustica::rtxdi_config::defaultPTHybridShift();
    RTXDI.restirPT.boilingFilterParams = caustica::rtxdi_config::defaultPTBoilingFilter();
    RTXDI.restirPT.spatialResamplingParams = caustica::rtxdi_config::defaultPTSpatialResampling();

    switch (RTXDIRestirPTPreset)
    {
    case RTXDIRestirPTQualityPreset::Fast:
        RTXDI.restirPT.resamplingMode = caustica::rtxdi_config::PTResamplingMode::Temporal;
        RTXDI.restirPT.initialSamplingParams.maxBounceDepth = 3;
        RTXDI.restirPT.initialSamplingParams.maxRcVertexLength = RTXDI.restirPT.initialSamplingParams.maxBounceDepth + 1;
        RTXDI.restirPT.initialSamplingParams.numInitialSamples = 1;
        RTXDI.restirPT.spatialResamplingParams.numDisocclusionBoostSamples = 2;
        RTXDI.restirPT.spatialResamplingParams.samplingRadius = 32.0f;
        RTXDI.restirPT.spatialResamplingParams.numSpatialSamples = 1;
        break;

    case RTXDIRestirPTQualityPreset::Medium:
        RTXDI.restirPT.resamplingMode = caustica::rtxdi_config::PTResamplingMode::TemporalAndSpatial;
        RTXDI.restirPT.initialSamplingParams.maxBounceDepth = 3;
        RTXDI.restirPT.initialSamplingParams.maxRcVertexLength = RTXDI.restirPT.initialSamplingParams.maxBounceDepth + 1;
        RTXDI.restirPT.initialSamplingParams.numInitialSamples = 1;
        RTXDI.restirPT.spatialResamplingParams.numDisocclusionBoostSamples = 4;
        RTXDI.restirPT.spatialResamplingParams.samplingRadius = 32.0f;
        RTXDI.restirPT.spatialResamplingParams.numSpatialSamples = 1;
        break;

    case RTXDIRestirPTQualityPreset::Ultra:
        RTXDI.restirPT.resamplingMode = caustica::rtxdi_config::PTResamplingMode::TemporalAndSpatial;
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

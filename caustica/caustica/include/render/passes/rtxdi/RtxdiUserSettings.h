#pragma once

#include <render/passes/rtxdi/RtxdiApplicationSettings.h>

struct RtxdiUserSettings
{
    caustica::rtxdi_config::CheckerboardMode checkerboardMode = caustica::rtxdi_config::CheckerboardMode::Off;

    struct
    {
        caustica::rtxdi_config::DIResamplingMode resamplingMode = caustica::rtxdi_config::DIResamplingMode::TemporalAndSpatial;
        caustica::rtxdi_config::DIInitialSamplingParameters initialSamplingParams = caustica::rtxdi_config::defaultDIInitialSampling();
        caustica::rtxdi_config::DITemporalResamplingParameters temporalResamplingParams = caustica::rtxdi_config::defaultDITemporalResampling();
        caustica::rtxdi_config::DISpatialResamplingParameters spatialResamplingParams = caustica::rtxdi_config::defaultDISpatialResampling();
        caustica::rtxdi_config::DIShadingParameters shadingParams = caustica::rtxdi_config::defaultDIShading();
    } restirDI;

    struct
    {
        caustica::rtxdi_config::GIResamplingMode resamplingMode = caustica::rtxdi_config::GIResamplingMode::TemporalAndSpatial;
        caustica::rtxdi_config::GITemporalResamplingParameters temporalResamplingParams = caustica::rtxdi_config::defaultGITemporalResampling();
        caustica::rtxdi_config::GISpatialResamplingParameters spatialResamplingParams = caustica::rtxdi_config::defaultGISpatialResampling();
        caustica::rtxdi_config::GIFinalShadingParameters finalShadingParams = caustica::rtxdi_config::defaultGIFinalShading();
    } restirGI;

    struct
    {
        caustica::rtxdi_config::PTResamplingMode resamplingMode = caustica::rtxdi_config::PTResamplingMode::TemporalAndSpatial;
        caustica::rtxdi_config::PTInitialSamplingParameters initialSamplingParams = caustica::rtxdi_config::defaultPTInitialSampling();
        caustica::rtxdi_config::PTTemporalResamplingParameters temporalResamplingParams = caustica::rtxdi_config::defaultPTTemporalResampling();
        caustica::rtxdi_config::PTReconnectionParameters reconnectionParams = caustica::rtxdi_config::defaultPTReconnection();
        caustica::rtxdi_config::PTHybridShiftParameters hybridShiftParams = caustica::rtxdi_config::defaultPTHybridShift();
        caustica::rtxdi_config::BoilingFilterParameters boilingFilterParams = caustica::rtxdi_config::defaultPTBoilingFilter();
        caustica::rtxdi_config::PTSpatialResamplingParameters spatialResamplingParams = caustica::rtxdi_config::defaultPTSpatialResampling();
    } restirPT;

    struct
    {
        caustica::rtxdi_config::ReGIRStaticParameters regirStaticParams{};
        caustica::rtxdi_config::ReGIRDynamicParameters regirDynamicParameters{};
    } regir;

    struct { int numIndirectSamples = 6; } regirIndirect;

    float rayEpsilon = 1.0e-4f;
    bool reStirGIEnableTemporalResampling = true;
    bool reStirGIVaryAgeThreshold = true;
};

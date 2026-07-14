#pragma once

#include <rtxdi/ImportanceSamplingContext.h>
#include <render/passes/rtxdi/RtxdiApplicationSettings.h>

// Lightweight settings POD for PathTracerSettings / SceneRenderData.
// Kept out of RtxdiPass.h so settings headers do not pull Scene.h (include cycle).

struct RtxdiUserSettings
{
    rtxdi::CheckerboardMode checkerboardMode = rtxdi::CheckerboardMode::Off;

    struct
    {
        rtxdi::ReSTIRDI_ResamplingMode resamplingMode = getReSTIRDI_ResamplingMode();
        ReSTIRDI_InitialSamplingParameters initialSamplingParams = getReSTIRDIInitialSamplingParams();
        ReSTIRDI_TemporalResamplingParameters temporalResamplingParams = getReSTIRDITemporalResamplingParams();
        ReSTIRDI_SpatialResamplingParameters spatialResamplingParams = getReSTIRDISpatialResamplingParams();
        ReSTIRDI_ShadingParameters shadingParams = getReSTIRDIShadingParams();
    } restirDI;

    struct
    {
        rtxdi::ReSTIRGI_ResamplingMode resamplingMode = getReSTIRGI_ResamplingMode();
        ReSTIRGI_TemporalResamplingParameters temporalResamplingParams = getReSTIRGITemporalResamplingParams();
        ReSTIRGI_SpatialResamplingParameters spatialResamplingParams = getReSTIRGISpatialResamplingParams();
        ReSTIRGI_FinalShadingParameters finalShadingParams = getReSTIRGIFinalShadingParams();
    } restirGI;

    struct
    {
        rtxdi::ReSTIRPT_ResamplingMode resamplingMode = getReSTIRPT_ResamplingMode();
        RTXDI_PTInitialSamplingParameters initialSamplingParams = getReSTIRPTInitialSamplingParams();
        RTXDI_PTTemporalResamplingParameters temporalResamplingParams = getReSTIRPTTemporalResamplingParams();
        RTXDI_PTReconnectionParameters reconnectionParams = getReSTIRPTReconnectionParams();
        RTXDI_PTHybridShiftPerFrameParameters hybridShiftParams = getReSTIRPTHybridShiftParams();
        RTXDI_BoilingFilterParameters boilingFilterParams = getReSTIRPTBoilingFilterParams();
        RTXDI_PTSpatialResamplingParameters spatialResamplingParams = getReSTIRPTSpatialResamplingParams();
    } restirPT;

    struct
    {
        rtxdi::ReGIRStaticParameters regirStaticParams = {};
        rtxdi::ReGIRDynamicParameters regirDynamicParameters = getReGIRDynamicParams();
    } regir;

    struct
    {
        int numIndirectSamples = 6;
    } regirIndirect;

    float rayEpsilon = 1.0e-4f;
    bool reStirGIEnableTemporalResampling = true;
    bool reStirGIVaryAgeThreshold = true;
};

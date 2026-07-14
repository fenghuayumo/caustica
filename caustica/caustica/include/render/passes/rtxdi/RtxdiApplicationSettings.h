#pragma once

#include <rtxdi/ImportanceSamplingContext.h>
#include <rtxdi/PT/ReSTIRPT.h>

// Set default values RTXDI settings
const rtxdi::ReSTIRDI_ResamplingMode getReSTIRDI_ResamplingMode();

const ReSTIRDI_InitialSamplingParameters getReSTIRDIInitialSamplingParams();

const ReSTIRDI_TemporalResamplingParameters getReSTIRDITemporalResamplingParams();

const ReSTIRDI_SpatialResamplingParameters getReSTIRDISpatialResamplingParams();

const ReSTIRDI_ShadingParameters getReSTIRDIShadingParams();

const rtxdi::ReSTIRGI_ResamplingMode getReSTIRGI_ResamplingMode();

const ReSTIRGI_TemporalResamplingParameters getReSTIRGITemporalResamplingParams();

const ReSTIRGI_SpatialResamplingParameters getReSTIRGISpatialResamplingParams();

const ReSTIRGI_FinalShadingParameters getReSTIRGIFinalShadingParams();

const rtxdi::ReGIRDynamicParameters getReGIRDynamicParams();

const rtxdi::ReSTIRPT_ResamplingMode getReSTIRPT_ResamplingMode();

const RTXDI_PTInitialSamplingParameters getReSTIRPTInitialSamplingParams();

const RTXDI_PTTemporalResamplingParameters getReSTIRPTTemporalResamplingParams();

const RTXDI_PTReconnectionParameters getReSTIRPTReconnectionParams();

const RTXDI_PTHybridShiftPerFrameParameters getReSTIRPTHybridShiftParams();

const RTXDI_BoilingFilterParameters getReSTIRPTBoilingFilterParams();

const RTXDI_PTSpatialResamplingParameters getReSTIRPTSpatialResamplingParams();

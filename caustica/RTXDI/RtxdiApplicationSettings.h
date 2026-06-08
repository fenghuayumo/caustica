/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#pragma once

#include <rtxdi/ImportanceSamplingContext.h>
#include <rtxdi/PT/ReSTIRPT.h>

// Set default values RTXDI settings
const rtxdi::ReSTIRDI_ResamplingMode GetReSTIRDI_ResamplingMode();

const ReSTIRDI_InitialSamplingParameters getReSTIRDIInitialSamplingParams();

const ReSTIRDI_TemporalResamplingParameters getReSTIRDITemporalResamplingParams();

const ReSTIRDI_SpatialResamplingParameters getReSTIRDISpatialResamplingParams();

const ReSTIRDI_ShadingParameters getReSTIRDIShadingParams();

const rtxdi::ReSTIRGI_ResamplingMode GetReSTIRGI_ResamplingMode();

const ReSTIRGI_TemporalResamplingParameters getReSTIRGITemporalResamplingParams();

const ReSTIRGI_SpatialResamplingParameters getReSTIRGISpatialResamplingParams();

const ReSTIRGI_FinalShadingParameters getReSTIRGIFinalShadingParams();

const rtxdi::ReGIRDynamicParameters getReGIRDynamicParams();

const rtxdi::ReSTIRPT_ResamplingMode GetReSTIRPT_ResamplingMode();

const RTXDI_PTInitialSamplingParameters getReSTIRPTInitialSamplingParams();

const RTXDI_PTTemporalResamplingParameters getReSTIRPTTemporalResamplingParams();

const RTXDI_PTReconnectionParameters getReSTIRPTReconnectionParams();

const RTXDI_PTHybridShiftPerFrameParameters getReSTIRPTHybridShiftParams();

const RTXDI_BoilingFilterParameters getReSTIRPTBoilingFilterParams();

const RTXDI_PTSpatialResamplingParameters getReSTIRPTSpatialResamplingParams();

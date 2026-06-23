/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#ifndef __SAMPLE_CONSTANT_BUFFER_H__
#define __SAMPLE_CONSTANT_BUFFER_H__

#if !defined(__cplusplus) // not needed in the port so far
#pragma pack_matrix(row_major) // matrices below are expected in row_major
#else
using namespace caustica::math;
#endif

#include "PathTracer/PathTracerShared.h"

#include "PathTracer/PathTracerDebug.hlsli"

#include "PathTracer/Lighting/LightingTypes.hlsli"
#include "PathTracer/Lighting/EnvMap.hlsli"

#define GAUSSIAN_SPLAT_SH_FLOAT4_COUNT 12

#define GAUSSIAN_SPLAT_SHADOWS_DISABLED 0
#define GAUSSIAN_SPLAT_SHADOWS_HARD 1
#define GAUSSIAN_SPLAT_SHADOWS_SOFT 2


struct SimpleViewConstants
{
    float4x4    matWorldToView;
    float4x4    matViewToClip;
    float4x4    matWorldToClip;
    float4x4    matWorldToClipNoOffset;
    float4x4    matClipToWorldNoOffset;
    // 
    float2      viewportOrigin;
    float2      viewportSize;
    // 
    float2      viewportSizeInv;
    float2      pixelOffset;
    // 
    float2      clipToWindowScale;
    float2      clipToWindowBias;
};

struct SampleConstants
{    
    SimpleViewConstants view;
    SimpleViewConstants previousView;
    EnvMapSceneParams envMapSceneParams;
    EnvMapImportanceSamplingParams envMapImportanceSamplingParams;
    PathTracerConstants ptConsts;
    DebugConstants debug;
    float4 denoisingHitParamConsts;

    uint MaterialCount;
    uint GaussianSplatShadowCount;
    uint GaussianSplatShadowsEnabled;
    float GaussianSplatShadowScale;

    float GaussianSplatShadowAlphaThreshold;
    uint GaussianSplatShadowUseTLASInstances;
    uint GaussianSplatShadowPrimitiveCountPerSplat;
    uint GaussianSplatShadowMode;

    float GaussianSplatShadowSoftRadius;
    uint GaussianSplatShadowSoftSampleCount;
    uint GaussianSplatShadowFrameIndex;
    float GaussianSplatShadowRayOffset;

    float GaussianSplatShadowAlphaScale;
    float GaussianSplatShadowKernelMinResponse;
    uint GaussianSplatShadowKernelDegree;
    uint GaussianSplatShadowAdaptiveClamp;

    float4x4 GaussianSplatShadowWorldToObject;
};

// Used in a couple of places like multipass postprocess where you want to keep SampleConstants the same for all passes, but send just a few additional per-pass parameters 
// In path tracing used to pass subSampleIndex (when enabled).
// Set as 'push constants' (root constants)
struct SampleMiniConstants
{
    uint4 params;
    uint4 params1;
    uint4 params2;
    uint4 params3;
};

struct GaussianSplatConstants
{
    SimpleViewConstants view;

    float4 cameraPosition;
    float4x4 objectToWorld;

    float splatScale;
    float alphaScale;
    float brightness;
    uint splatCount;

    float3 tintColor;
    float alphaCullThreshold;

    uint shDegree;
    uint depthTest;
    uint shadowsEnabled;
    uint padding0;

    float4 shadowDirectionToLight;

    float shadowStrength;
    float shadowRayTMax;
    uint shadowMode;
    uint shadowSoftSampleCount;

    float shadowSoftRadius;
    uint shadowFrameIndex;
    uint sortMode;
    uint frustumCulling;

    float frustumDilation;
    float minPixelCoverage;
    uint screenSizeCulling;
    uint mipSplattingAntialiasing;

    uint shFormat;
    uint rgbaFormat;
    uint projectionMethod;
    uint stochasticFrameIndex;
};

struct GaussianSplatData
{
    float4 centerOpacity; // xyz = center, w = opacity after sigmoid
    float4 covariance0;   // xx, xy, xz, yy
    float4 covariance1;   // yz, zz, unused, unused
    float4 color;         // rgb = SH degree 0 color, a unused
};

#endif // __SAMPLE_CONSTANT_BUFFER_H__

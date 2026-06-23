#ifndef __LIGHTING_BINDINGS_HLSLI__    // using instead of "#pragma once" due to https://github.com/microsoft/DirectXShaderCompiler/issues/3943
#define __LIGHTING_BINDINGS_HLSLI__

#include <shaders/bindless.h>
#include <shaders/binding_helpers.hlsli>

#include "../PathTracer/Lighting/LightingTypes.hlsli"

// Bindings 10-19 are scene lighting: environment map (distant lights) sampling, local lights sampling, etc.
TextureCube<float4> t_EnvironmentMap                                                : register(t10);
Texture2D<float>    t_EnvironmentMapImportanceMap                                   : register(t11);

StructuredBuffer<LightingControlData>       t_LightsCB                              : register(t12);
StructuredBuffer<PolymorphicLightInfo>      t_Lights                                : register(t13);
StructuredBuffer<PolymorphicLightInfoEx>    t_LightsEx                              : register(t14);

Buffer<uint>                                t_LightProxyCounters                    : register(t15);
Buffer<uint>                                t_LightProxyIndices                     : register(t16);
Buffer<uint>                                t_LightLocalSamplingBuffer              : register(t17);
Texture2D<uint>                             t_EnvLookupMap                          : register(t18);

RWTexture2D<float>                          u_LightFeedbackTotalWeight              : register(u20);
RWTexture2D<uint>  u_LightFeedbackCandidates               : register(u21);

#endif //__LIGHTING_BINDINGS_HLSLI__

#ifndef __SAMPLER_BINDINGS_HLSLI__    // using instead of "#pragma once" due to https://github.com/microsoft/DirectXShaderCompiler/issues/3943
#define __SAMPLER_BINDINGS_HLSLI__

#include <shaders/bindless.h>
#include <shaders/binding_helpers.hlsli>

SamplerState s_MaterialSampler                              : register(s0);
SamplerState s_EnvironmentMapSampler                        : register(s1);
SamplerState s_EnvironmentMapImportanceSampler              : register(s2);

#endif //__SAMPLER_BINDINGS_HLSLI__

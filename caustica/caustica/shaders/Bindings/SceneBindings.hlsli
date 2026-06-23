#ifndef __SCENE_BINDINGS_HLSLI__    // using instead of "#pragma once" due to https://github.com/microsoft/DirectXShaderCompiler/issues/3943
#define __SCENE_BINDINGS_HLSLI__

#include <shaders/bindless.h>
#include <shaders/binding_helpers.hlsli>

#include "../SubInstanceData.h"
#include "../PathTracer/Materials/MaterialPT.h"
#include "../Misc/OmmGeometryDebugData.hlsli"

// Bindings 0-9 are scene data: acceleration structures, geometry & instance data, materials, etc.
RaytracingAccelerationStructure SceneBVH                    : register(t0);
StructuredBuffer<SubInstanceData> t_SubInstanceData         : register(t1);
StructuredBuffer<InstanceData> t_InstanceData               : register(t2);
StructuredBuffer<GeometryData> t_GeometryData               : register(t3);
StructuredBuffer<GeometryDebugData> t_GeometryDebugData     : register(t4);
StructuredBuffer<PTMaterialData> t_PTMaterialData           : register(t5);
RaytracingAccelerationStructure GaussianSplatBVH            : register(t7);
StructuredBuffer<GaussianSplatData> t_GaussianShadowSplats  : register(t8);

// Bindless
VK_BINDING(0, 1) ByteAddressBuffer t_BindlessBuffers[]      : register(t0, space1);
VK_BINDING(1, 1) Texture2D t_BindlessTextures[]             : register(t0, space2);

#endif //__SCENE_BINDINGS_HLSLI__

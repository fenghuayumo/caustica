#ifndef SET_SURFACE_DATA_HLSLI
#define SET_SURFACE_DATA_HLSLI

#include "SurfaceData.hlsli"
#include "ShaderParameters.h"

ConstantBuffer<RtxdiBridgeConstants> g_RtxdiBridgeConst     : register(b2 VK_DESCRIPTOR_SET(2));


#endif // SET_SURFACE_DATA_HLSLI

#ifndef VULKAN_HLSLI
#define VULKAN_HLSLI

#if defined(__HLSL_VERSION) // FXC doesn't understand #warning
#warning "vulkan.hlsli is deprecated - please update the code to use binding_helpers.hlsli"
#endif

#include "binding_helpers.hlsli"

#endif // VULKAN_HLSLI
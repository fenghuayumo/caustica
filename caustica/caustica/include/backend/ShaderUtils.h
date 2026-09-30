#pragma once

#include <rhi/rhi.h>

namespace caustica
{

// Returns "dxil" or "spirv" based on the graphics API.
inline const char* getShaderTypeName(caustica::rhi::GraphicsAPI api)
{
    switch (api)
    {
    case caustica::rhi::GraphicsAPI::D3D12:  return "dxil";
    case caustica::rhi::GraphicsAPI::VULKAN: return "spirv";
    default:                         return "";
    }
}

} // namespace caustica

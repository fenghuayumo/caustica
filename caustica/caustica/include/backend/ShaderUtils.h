#pragma once

#include <rhi/nvrhi.h>

namespace caustica
{

// Returns "dxil", "dxbc", or "spirv" based on the graphics API.
inline const char* getShaderTypeName(nvrhi::GraphicsAPI api)
{
    switch (api)
    {
    case nvrhi::GraphicsAPI::D3D11:  return "dxbc";
    case nvrhi::GraphicsAPI::D3D12:  return "dxil";
    case nvrhi::GraphicsAPI::VULKAN: return "spirv";
    default:                         return "";
    }
}

} // namespace caustica

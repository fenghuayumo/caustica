#include <assets/loader/ShaderBackend.h>

#include <rhi/nvrhi.h>

namespace caustica::shader
{

Backend fromNvrhiGraphicsApi(nvrhi::GraphicsAPI api)
{
    switch (api)
    {
    case nvrhi::GraphicsAPI::D3D11:  return Backend::D3D11;
    case nvrhi::GraphicsAPI::D3D12:  return Backend::D3D12;
    case nvrhi::GraphicsAPI::VULKAN: return Backend::Vulkan;
    default:                         return Backend::D3D12;
    }
}

nvrhi::GraphicsAPI toNvrhiGraphicsApi(Backend backend)
{
    switch (backend)
    {
    case Backend::D3D11:  return nvrhi::GraphicsAPI::D3D11;
    case Backend::D3D12:  return nvrhi::GraphicsAPI::D3D12;
    case Backend::Vulkan: return nvrhi::GraphicsAPI::VULKAN;
    }
    return nvrhi::GraphicsAPI::D3D12;
}

const char* backendToken(Backend backend)
{
    switch (backend)
    {
    case Backend::D3D11:  return "d3d11";
    case Backend::D3D12:  return "d3d12";
    case Backend::Vulkan: return "vulkan";
    }
    return "unknown";
}

} // namespace caustica::shader

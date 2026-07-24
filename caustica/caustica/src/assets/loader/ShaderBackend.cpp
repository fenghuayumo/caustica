#include <assets/loader/ShaderBackend.h>

namespace caustica::shader
{

Backend fromRhiGraphicsApi(caustica::rhi::GraphicsAPI api)
{
    switch (api)
    {
    case caustica::rhi::GraphicsAPI::D3D11:  return Backend::D3D11;
    case caustica::rhi::GraphicsAPI::D3D12:  return Backend::D3D12;
    case caustica::rhi::GraphicsAPI::VULKAN: return Backend::Vulkan;
    default:                         return Backend::D3D12;
    }
}

caustica::rhi::GraphicsAPI toRhiGraphicsApi(Backend backend)
{
    switch (backend)
    {
    case Backend::D3D11:  return caustica::rhi::GraphicsAPI::D3D11;
    case Backend::D3D12:  return caustica::rhi::GraphicsAPI::D3D12;
    case Backend::Vulkan: return caustica::rhi::GraphicsAPI::VULKAN;
    }
    return caustica::rhi::GraphicsAPI::D3D12;
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

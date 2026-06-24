#pragma once

#include <rhi/nvrhi.h>

namespace caustica
{

// Parse -d3d11/-dx11, -d3d12/-dx12, -vk/-vulkan flags from argv.
// Returns the detected API, or a build-time default.
inline nvrhi::GraphicsAPI GetGraphicsAPIFromCommandLine(int argc, const char* const* argv)
{
    for (int n = 1; n < argc; n++)
    {
        const char* arg = argv[n];

        if (!strcmp(arg, "-d3d11") || !strcmp(arg, "-dx11") ||
            !strcmp(arg, "--d3d11") || !strcmp(arg, "--dx11"))
            return nvrhi::GraphicsAPI::D3D11;
        else if (!strcmp(arg, "-d3d12") || !strcmp(arg, "-dx12") ||
                 !strcmp(arg, "--d3d12") || !strcmp(arg, "--dx12"))
            return nvrhi::GraphicsAPI::D3D12;
        else if (!strcmp(arg, "-vk") || !strcmp(arg, "-vulkan") ||
                 !strcmp(arg, "--vk") || !strcmp(arg, "--vulkan"))
            return nvrhi::GraphicsAPI::VULKAN;
    }

#if CAUSTICA_WITH_DX12
    return nvrhi::GraphicsAPI::D3D12;
#elif CAUSTICA_WITH_VULKAN
    return nvrhi::GraphicsAPI::VULKAN;
#elif CAUSTICA_WITH_DX11
    return nvrhi::GraphicsAPI::D3D11;
#else
    #error "No Graphics API defined"
#endif
}

} // namespace caustica

#pragma once

#include <cstring>
#include <rhi/rhi.h>

namespace caustica
{

// Parse -d3d11/-dx11, -d3d12/-dx12, -vk/-vulkan flags from argv.
// Returns the detected API, or a build-time default.
inline caustica::rhi::GraphicsAPI getGraphicsAPIFromCommandLine(int argc, const char* const* argv)
{
    for (int n = 1; n < argc; n++)
    {
        const char* arg = argv[n];

        if (!strcmp(arg, "-d3d11") || !strcmp(arg, "-dx11") ||
            !strcmp(arg, "--d3d11") || !strcmp(arg, "--dx11"))
            return caustica::rhi::GraphicsAPI::D3D11;
        else if (!strcmp(arg, "-d3d12") || !strcmp(arg, "-dx12") ||
                 !strcmp(arg, "--d3d12") || !strcmp(arg, "--dx12"))
            return caustica::rhi::GraphicsAPI::D3D12;
        else if (!strcmp(arg, "-vk") || !strcmp(arg, "-vulkan") ||
                 !strcmp(arg, "--vk") || !strcmp(arg, "--vulkan"))
            return caustica::rhi::GraphicsAPI::VULKAN;
    }

#if CAUSTICA_WITH_DX12
    return caustica::rhi::GraphicsAPI::D3D12;
#elif CAUSTICA_WITH_VULKAN
    return caustica::rhi::GraphicsAPI::VULKAN;
#elif CAUSTICA_WITH_DX11
    return caustica::rhi::GraphicsAPI::D3D11;
#else
    #error "No Graphics API defined"
#endif
}

// Extended parser that also accepts --backend= / --api= style overrides.
caustica::rhi::GraphicsAPI resolveGraphicsAPIFromCommandLine(int argc, const char* const* argv);

} // namespace caustica

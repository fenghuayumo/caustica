#include <engine/cmdline_utils.h>

#include <core/log.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace caustica
{

namespace
{
    std::string LowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });
        return value;
    }

    bool IsTrueOptionValue(const std::string& value)
    {
        return value.empty()
            || value == "1"
            || value == "true"
            || value == "yes"
            || value == "on";
    }

    bool TryParseBackendName(const std::string& value, nvrhi::GraphicsAPI& api)
    {
        const std::string backend = LowerAscii(value);
        if (backend == "vk" || backend == "vulkan")
        {
            api = nvrhi::GraphicsAPI::VULKAN;
            return true;
        }
        if (backend == "dx12" || backend == "d3d12" || backend == "directx12" || backend == "directx")
        {
            api = nvrhi::GraphicsAPI::D3D12;
            return true;
        }
        if (backend == "dx11" || backend == "d3d11" || backend == "directx11")
        {
            api = nvrhi::GraphicsAPI::D3D11;
            return true;
        }
        return false;
    }
} // namespace

nvrhi::GraphicsAPI ResolveGraphicsAPIFromCommandLine(int argc, const char* const* argv)
{
#if defined(_WIN32)
    nvrhi::GraphicsAPI api = GetGraphicsAPIFromCommandLine(argc, argv);

    for (int n = 1; n < argc; ++n)
    {
        std::string arg = argv[n] ? argv[n] : "";
        std::string key = arg;
        std::string value;

        const size_t equals = key.find('=');
        if (equals != std::string::npos)
        {
            value = LowerAscii(key.substr(equals + 1));
            key = key.substr(0, equals);
        }

        key = LowerAscii(key);

        if (key == "-vk" || key == "--vk" || key == "-vulkan" || key == "--vulkan")
        {
            if (IsTrueOptionValue(value))
                api = nvrhi::GraphicsAPI::VULKAN;
        }
        else if (key == "-d3d12" || key == "--d3d12" || key == "-dx12" || key == "--dx12")
        {
            if (IsTrueOptionValue(value))
                api = nvrhi::GraphicsAPI::D3D12;
        }
        else if (key == "--backend" || key == "--api" || key == "--graphicsapi")
        {
            std::string backend = value;
            if (backend.empty() && n + 1 < argc)
                backend = argv[++n] ? argv[n] : "";

            nvrhi::GraphicsAPI parsedApi;
            if (TryParseBackendName(backend, parsedApi))
                api = parsedApi;
            else
                warning("Unknown render backend '%s'. Falling back to the default backend.", backend.c_str());
        }
    }

    return api;
#else
    (void)argc;
    (void)argv;
    return nvrhi::GraphicsAPI::VULKAN;
#endif
}

} // namespace caustica

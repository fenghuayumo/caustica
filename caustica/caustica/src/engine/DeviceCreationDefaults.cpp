#include <backend/GpuDevice.h>
#include <backend/GpuSurface.h>

#include <core/log.h>
#include <core/progress.h>
#include <platform/glfw_window.h>
#include <platform/window.h>

#include <algorithm>
#include <memory>
#include <vector>

#if CAUSTICA_WITH_NATIVE_DLSS
#include <render/passes/geometry/DLSS.h>
#endif

namespace caustica
{

namespace
{
    constexpr uint32_t kDefaultSwapChainBufferCount = 3;

    void AppendUnique(std::vector<std::string>& values, const std::string& value)
    {
        if (std::find(values.begin(), values.end(), value) == values.end())
            values.push_back(value);
    }

    void ApplyPathTracerVulkanDefaults(DeviceCreationParameters& params)
    {
#if CAUSTICA_WITH_VULKAN
#if CAUSTICA_WITH_NATIVE_DLSS
        render::DLSS::getRequiredVulkanExtensions(
            params.requiredVulkanInstanceExtensions,
            params.requiredVulkanDeviceExtensions);
#endif
        AppendUnique(params.requiredVulkanDeviceExtensions, "VK_KHR_buffer_device_address");
        AppendUnique(params.requiredVulkanDeviceExtensions, "VK_KHR_format_feature_flags2");

        params.ignoredVulkanValidationMessageLocations.push_back(0x0000000023e43bb7);
        params.ignoredVulkanValidationMessageLocations.push_back(0x000000000609a13b);
        params.ignoredVulkanValidationMessageLocations.push_back(0x00000000c5a3822a);
        params.ignoredVulkanValidationMessageLocations.push_back(0x00000000591f70f2);
        params.ignoredVulkanValidationMessageLocations.push_back(0x000000005e6e827d);
#endif
    }

    void ApplyPathTracerEngineDefaults(DeviceCreationParameters& params)
    {
        params.swapChainSampleCount = 1;
        params.swapChainBufferCount = kDefaultSwapChainBufferCount;
        params.vsyncEnabled = true;
        params.enableRayTracingExtensions = true;
        params.requirePathTracerFeatures = true;
        params.supportExplicitDisplayScaling = true;
        params.enablePerMonitorDPI = true;

#if CAUSTICA_WITH_DX12
#if defined(CAUSTICA_D3D_AGILITY_SDK_VERSION)
        params.featureLevel = D3D_FEATURE_LEVEL_12_2;
#else
        params.featureLevel = D3D_FEATURE_LEVEL_12_1;
#endif
#endif

#if defined(_DEBUG)
        params.enableDebugRuntime = true;
        params.enableWarningsAsErrors = true;
        params.enableRhiValidationLayer = true;
        params.enableGPUValidation = false;
#endif

#if CAUSTICA_WITH_STREAMLINE
        params.checkStreamlineSignature = true;
        params.streamlineAppId = 231313132;
#if defined(_DEBUG)
        params.enableStreamlineLog = true;
#endif
#endif

        ApplyPathTracerVulkanDefaults(params);
    }

    DeviceCreationParameters MakeBackendParams(const GpuDeviceCreateDesc& desc)
    {
        DeviceCreationParameters params;
        ApplyPathTracerEngineDefaults(params);

        params.backBufferWidth = desc.backBufferWidth;
        params.backBufferHeight = desc.backBufferHeight;
        if (desc.swapChainBufferCount != 0)
            params.swapChainBufferCount = desc.swapChainBufferCount;
        params.startFullscreen = desc.startFullscreen;
        params.startMaximized = desc.startMaximized;
        params.startBorderless = desc.startBorderless;
        params.vsyncEnabled = desc.vsyncEnabled;
        params.adapter = desc.adapter;
        params.headlessDevice = desc.headless;

        if (desc.enableDebug)
        {
            params.enableDebugRuntime = true;
            params.enableRhiValidationLayer = true;
        }

#if CAUSTICA_WITH_DX12
        if (desc.d3d12DeviceFactory)
            params.d3d12DeviceFactory = desc.d3d12DeviceFactory;
#endif

        return params;
    }

    std::unique_ptr<Window> CreatePlatformWindow(const GpuDeviceCreateDesc& desc)
    {
        GlfwWindow::makeDefault();

        WindowDesc windowDesc;
        windowDesc.Width = desc.backBufferWidth;
        windowDesc.Height = desc.backBufferHeight;
        windowDesc.Fullscreen = desc.startFullscreen;
        windowDesc.Maximized = desc.startMaximized && !desc.startFullscreen;
        windowDesc.Borderless = desc.startBorderless;
        windowDesc.VSync = desc.vsyncEnabled;
        windowDesc.Title = desc.windowTitle;
        windowDesc.RenderAPI = static_cast<int>(desc.api);

        std::unique_ptr<Window> window(Window::create(windowDesc));
        if (!window || !window->hasInitialised())
            return nullptr;
        return window;
    }
} // namespace

GpuDeviceCreateResult GpuDevice::create(const GpuDeviceCreateDesc& desc)
{
    GpuDeviceCreateResult result;
    const DeviceCreationParameters backendParams = MakeBackendParams(desc);

    // 1. Backend object (DX12 / Vulkan / DX11).
    std::unique_ptr<GpuDevice> gpuDevice(GpuDevice::createBackend(desc.api));
    if (!gpuDevice)
    {
        caustica::error("GpuDevice: no backend for the requested graphics API");
        return result;
    }

    gpuDevice->setFrameTimeUpdateInterval(1.0);

    if (desc.headless)
    {
        if (!gpuDevice->createHeadlessTargets(backendParams))
        {
            caustica::error("GpuDevice: failed to create headless device / targets");
            return result;
        }

        result.surface = GpuSurface::createHeadless(*gpuDevice);
        if (!result.surface)
        {
            caustica::error("GpuDevice: failed to create headless surface");
            return result;
        }

        result.gpuDevice = std::move(gpuDevice);
        return result;
    }

    std::unique_ptr<Window> window = CreatePlatformWindow(desc);
    if (!window)
    {
        caustica::error("GpuDevice: failed to create platform window");
        return result;
    }

    if (!gpuDevice->bindPresentTarget(*window))
    {
        caustica::error("GpuDevice: failed to bind present target");
        return result;
    }

    if (!gpuDevice->createLogicalDevice(backendParams))
    {
        caustica::error("GpuDevice: failed to create logical device");
        return result;
    }

    result.surface = GpuSurface::createWindowed(*gpuDevice, *window);
    if (!result.surface)
    {
        caustica::error("GpuDevice: failed to create window surface");
        return result;
    }

    if (desc.startMaximized && !desc.startFullscreen)
        window->maximise();

    helpersRegisterActiveWindow(window->getNativeHandle());
    result.gpuDevice = std::move(gpuDevice);
    result.window = std::move(window);
    return result;
}

} // namespace caustica

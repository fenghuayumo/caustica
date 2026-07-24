#include <backend/GpuDevice.h>

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

        // OMM baker / validation-layer noise that is benign on our targets.
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

    DeviceCreationParameters MakePathTracerDeviceParameters(const GpuDeviceCreateDesc& desc)
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
        params.adapterIndex = desc.adapterIndex;
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
} // namespace

GpuDeviceCreateResult GpuDevice::createInitialized(const GpuDeviceCreateDesc& desc)
{
    GpuDeviceCreateResult result;
    const DeviceCreationParameters deviceParams = MakePathTracerDeviceParameters(desc);

    std::unique_ptr<GpuDevice> gpuDevice(GpuDevice::create(desc.api));
    if (!gpuDevice)
    {
        caustica::error("GpuDevice::createInitialized: GpuDevice::create returned null");
        return result;
    }

    gpuDevice->setFrameTimeUpdateInterval(1.0);

    if (desc.headless)
    {
        if (!gpuDevice->initializeHeadlessGraphics(deviceParams))
        {
            caustica::error("GpuDevice::createInitialized: failed to create headless graphics device");
            return result;
        }

        result.gpuDevice = std::move(gpuDevice);
        return result;
    }

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
    {
        caustica::error("GpuDevice::createInitialized: failed to create platform window");
        return result;
    }

    if (windowDesc.Maximized)
        window->maximise();

    if (!gpuDevice->initializeGraphicsDevice(deviceParams))
    {
        caustica::error("GpuDevice::createInitialized: failed to create graphics device");
        return result;
    }

    if (!gpuDevice->initializeWindowSwapChain(window.get()))
    {
        caustica::error("GpuDevice::createInitialized: failed to create swap chain");
        return result;
    }

    helpersRegisterActiveWindow(window->getNativeHandle());
    result.gpuDevice = std::move(gpuDevice);
    result.window = std::move(window);
    return result;
}

} // namespace caustica

#pragma once

// Backend-only create knobs. Hosts use GpuDeviceCreateDesc.
// Instance → adapter → logical device → optional surface.

#if CAUSTICA_WITH_DX11 || CAUSTICA_WITH_DX12
#include <DXGI.h>
#endif

#if CAUSTICA_WITH_DX11
#include <d3d11.h>
#endif

#if CAUSTICA_WITH_DX12
#include <d3d12.h>
#endif

#if CAUSTICA_WITH_VULKAN
#include <rhi/vulkan.h>
#endif

#include <rhi/rhi.h>
#include <core/log.h>

#include <functional>
#include <string>
#include <vector>

namespace caustica
{

struct InstanceParameters
{
    bool enableDebugRuntime = false;
    bool enableWarningsAsErrors = false;
    bool enableGPUValidation = false;
    bool headlessDevice = false;
#if CAUSTICA_WITH_AFTERMATH
    bool enableAftermath = false;
#endif
    bool logBufferLifetime = false;
    bool enableHeapDirectlyIndexed = false;
    bool enablePerMonitorDPI = false;
    caustica::Severity infoLogSeverity = caustica::Severity::Info;

#if CAUSTICA_WITH_VULKAN
    std::string vulkanLibraryName;
    std::vector<std::string> requiredVulkanInstanceExtensions;
    std::vector<std::string> requiredVulkanLayers;
    std::vector<std::string> optionalVulkanInstanceExtensions;
    std::vector<std::string> optionalVulkanLayers;
#endif

#if CAUSTICA_WITH_STREAMLINE
    bool enableStreamline = true;
    int streamlineAppId = 1;
    bool checkStreamlineSignature = true;
    bool enableStreamlineLog = false;
#endif
};

struct DeviceCreationParameters : public InstanceParameters
{
    bool startMaximized = false;
    bool startFullscreen = false;
    bool startBorderless = false;
    bool allowModeSwitch = false;
    int windowPosX = -1;
    int windowPosY = -1;
    uint32_t backBufferWidth = 1280;
    uint32_t backBufferHeight = 720;
    uint32_t refreshRate = 0;
    uint32_t swapChainBufferCount = 3;
    caustica::rhi::Format swapChainFormat = caustica::rhi::Format::SRGBA8_UNORM;
    uint32_t swapChainSampleCount = 1;
    uint32_t swapChainSampleQuality = 0;
    caustica::rhi::Format depthBufferFormat = caustica::rhi::Format::UNKNOWN;

    uint32_t maxFramesInFlight = 2;
    bool enableRhiValidationLayer = false;
    bool vsyncEnabled = false;
    bool enableRayTracingExtensions = false;
    bool requirePathTracerFeatures = true;
    bool enableComputeQueue = false;
    bool enableCopyQueue = false;
    caustica::rhi::AdapterSelector adapter;
    bool supportExplicitDisplayScaling = false;
    bool resizeWindowWithDisplayScale = false;
    caustica::rhi::MessageCallback* messageCallback = nullptr;

#if CAUSTICA_WITH_DX11 || CAUSTICA_WITH_DX12
    DXGI_USAGE swapChainUsage = DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT;
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
#endif

#if CAUSTICA_WITH_DX12
    ID3D12DeviceFactory* d3d12DeviceFactory = nullptr;
#endif

#if CAUSTICA_WITH_VULKAN
    std::vector<std::string> requiredVulkanDeviceExtensions;
    std::vector<std::string> optionalVulkanDeviceExtensions;
    std::vector<size_t> ignoredVulkanValidationMessageLocations = {
        0x13365b2
    };
    std::function<void(VkDeviceCreateInfo&)> deviceCreateInfoCallback;
    void* physicalDeviceFeatures2Extensions = nullptr;
#endif
};

} // namespace caustica

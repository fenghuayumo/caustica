#pragma once

#include <vector>
#include <string>
#include <memory>
#include <functional>

// Forward: nvrhi
namespace nvrhi {
    class IDevice;
    class ISwapchain;
    enum class GraphicsAPI : int;
}

namespace caustica {

// =============================================================================
// GpuDevice — Backend layer: GPU adapter enumeration, instance/device creation.
//
// Extracted from DeviceManager. Wraps NVRHI device creation and owns the
// graphics API instance. Does NOT own the swapchain (see SwapChain class).
// =============================================================================

struct AdapterInfo
{
    std::string name;
    uint32_t    vendorId = 0;
    uint32_t    deviceId = 0;
    uint64_t    dedicatedVideoMemory = 0;
    int         adapterIndex = 0;
    bool        isRayTracingSupported = false;
};

struct DeviceCreationParameters
{
    // Back buffer
    int backBufferWidth  = 0;
    int backBufferHeight = 0;
    int swapChainSampleCount = 1;
    int swapChainBufferCount = 3;
    int refreshRate = 0;

    // Window
    bool startFullscreen   = false;
    bool startBorderless   = false;
    bool allowModeSwitch   = false;
    bool vsyncEnabled      = true;
    bool headlessDevice    = false;
    int  adapterIndex      = -1;
    int  windowPosX        = -1;
    int  windowPosY        = -1;

    // Debug
    bool enableDebugRuntime       = false;
    bool enableWarningsAsErrors   = false;
    bool enableGPUValidation      = false;
    bool enableNvrhiValidationLayer = false;
    bool enableAftermath          = false;
    bool logBufferLifetime        = false;

    // Features
    bool enableRayTracingExtensions = true;
    bool enablePerMonitorDPI        = true;
    bool supportExplicitDisplayScaling = false;
    bool resizeWindowWithDisplayScale = false;

    // Streamline
    bool checkStreamlineSignature  = false;
    int  streamlineAppId          = 0;
    bool enableStreamlineLog      = false;

    // Vulkan
    std::vector<std::string> requiredVulkanInstanceExtensions;
    std::vector<std::string> requiredVulkanDeviceExtensions;
    std::vector<size_t>      ignoredVulkanValidationMessageLocations;

    // Advanced
    std::function<void(void*)> deviceCreateInfoCallback;
    void* physicalDeviceFeatures2Extensions = nullptr;
};

class GpuDevice
{
public:
    GpuDevice() = default;
    ~GpuDevice();

    // --- Lifecycle ---
    bool createInstance(const DeviceCreationParameters& params);
    bool createDevice(const DeviceCreationParameters& params);
    void shutdown();

    bool isCreated() const { return m_Device != nullptr; }

    // --- Adapter enumeration ---
    bool enumerateAdapters(std::vector<AdapterInfo>& outAdapters);

    // --- Accessors ---
    nvrhi::IDevice*      device()    const { return m_Device; }
    nvrhi::GraphicsAPI   graphicsAPI() const;

    const DeviceCreationParameters& params() const { return m_Params; }

    // --- DPI ---
    void getDPIScale(float& x, float& y) const { x = m_DPIScaleX; y = m_DPIScaleY; }
    void setDPIScale(float x, float y)       { m_DPIScaleX = x; m_DPIScaleY = y; }

private:
    nvrhi::IDevice* m_Device = nullptr;
    DeviceCreationParameters m_Params;
    float m_DPIScaleX = 1.0f;
    float m_DPIScaleY = 1.0f;
    bool  m_InstanceCreated = false;

    // Platform-specific instance data (DXGI factory, Vulkan instance, etc.)
    void* m_InstanceData = nullptr;

    bool createInstanceInternal();
};

} // namespace caustica

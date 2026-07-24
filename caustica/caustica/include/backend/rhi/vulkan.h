#pragma once

#include <vulkan/vulkan.h>
#include <rhi/rhi_types.h>

namespace caustica::rhi
{
    namespace ObjectTypes
    {
        constexpr ObjectType CAUSTICA_RHI_VK_Device = 0x00030101;
    };
}

namespace caustica::rhi::vulkan
{
    // Vulkan-specific Device methods live on the concrete backend type
    // (caustica::rhi::vulkan::Device). Use checked_cast when needed.
    typedef caustica::rhi::DeviceHandle DeviceHandle;

    struct DeviceDesc
    {
        MessageCallback* errorCB = nullptr;

        VkInstance instance;
        VkPhysicalDevice physicalDevice;
        VkDevice device;

        // any of the queues can be null if this context doesn't intend to use them
        VkQueue graphicsQueue;
        int graphicsQueueIndex = -1;
        VkQueue transferQueue;
        int transferQueueIndex = -1;
        VkQueue computeQueue;
        int computeQueueIndex = -1;

        VkAllocationCallbacks *allocationCallbacks = nullptr;

        const char **instanceExtensions = nullptr;
        size_t numInstanceExtensions = 0;
        
        const char **deviceExtensions = nullptr;
        size_t numDeviceExtensions = 0;

        uint32_t maxTimerQueries = 256;

        // Indicates if VkPhysicalDeviceVulkan12Features::bufferDeviceAddress was set to 'true' at device creation time
        bool bufferDeviceAddressSupported = false;
        bool aftermathEnabled = false;
        bool logBufferLifetime = false;

        std::string vulkanLibraryName; // if empty, use default
    };

    CAUSTICA_RHI_API DeviceHandle createDevice(const DeviceDesc& desc);
   
    CAUSTICA_RHI_API VkFormat convertFormat(caustica::rhi::Format format);

    CAUSTICA_RHI_API const char* resultToString(VkResult result);
}


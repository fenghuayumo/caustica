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
    class IDevice : public caustica::rhi::IDevice
    {
    public:
        // Additional Vulkan-specific public methods
        virtual VkSemaphore getQueueSemaphore(CommandQueue queue) = 0;
        virtual void queueWaitForSemaphore(CommandQueue waitQueue, VkSemaphore semaphore, uint64_t value) = 0;
        virtual void queueSignalSemaphore(CommandQueue executionQueue, VkSemaphore semaphore, uint64_t value) = 0;
        virtual uint64_t queueGetCompletedInstance(CommandQueue queue) = 0;
    };

    typedef RefCountPtr<IDevice> DeviceHandle;

    struct DeviceDesc
    {
        IMessageCallback* errorCB = nullptr;

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


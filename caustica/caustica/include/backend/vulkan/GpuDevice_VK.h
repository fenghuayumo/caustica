#pragma once

#include <string>
#include <queue>
#include <unordered_set>

#include <backend/GpuDevice.h>
#include <core/log.h>

#include <rhi/vulkan.h>
#include <rhi/validation.h>

#ifndef VULKAN_HPP_DISPATCH_LOADER_DYNAMIC
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#endif
#include <vulkan/vulkan.hpp>

class GpuDevice_VK : public caustica::GpuDevice
{
public:
    [[nodiscard]] nvrhi::IDevice* getDevice() const override
    {
        if (m_ValidationLayer)
            return m_ValidationLayer;

        return m_NvrhiDevice;
    }
    
    [[nodiscard]] nvrhi::GraphicsAPI getGraphicsAPI() const override
    {
        return nvrhi::GraphicsAPI::VULKAN;
    }

    bool enumerateAdapters(std::vector<caustica::AdapterInfo>& outAdapters) override;
    [[nodiscard]] bool shouldIgnoreValidationMessageLocation(size_t location) const;

protected:
    bool createInstanceInternal() override;
    bool createDevice() override;
    bool createSwapChain() override;
    void destroyDeviceAndSwapChain() override;

    void resizeSwapChain() override
    {
        if (m_DeviceParams.headlessDevice)
            return;

        if (m_VulkanDevice)
        {
            destroySwapChain();
            createSwapChain();
        }
    }

    nvrhi::ITexture* getCurrentBackBuffer() override
    {
        if (m_DeviceParams.headlessDevice)
            return getHeadlessBackBuffer(getCurrentHeadlessBackBufferIndex());

        return m_SwapChainImages[m_SwapChainIndex].rhiHandle;
    }
    nvrhi::ITexture* getBackBuffer(uint32_t index) override
    {
        if (m_DeviceParams.headlessDevice)
            return getHeadlessBackBuffer(index);

        if (index < m_SwapChainImages.size())
            return m_SwapChainImages[index].rhiHandle;
        return nullptr;
    }
    uint32_t getCurrentBackBufferIndex() override
    {
        if (m_DeviceParams.headlessDevice)
            return getCurrentHeadlessBackBufferIndex();

        return m_SwapChainIndex;
    }
    uint32_t getBackBufferCount() override
    {
        if (m_DeviceParams.headlessDevice)
            return getHeadlessBackBufferCount();

        return uint32_t(m_SwapChainImages.size());
    }

    bool beginFrame() override;
    bool present() override;

    const char *getRendererString() const override
    {
        return m_RendererString.c_str();
    }

    bool isVulkanInstanceExtensionEnabled(const char* extensionName) const override
    {
        return enabledExtensions.instance.find(extensionName) != enabledExtensions.instance.end();
    }

    bool isVulkanDeviceExtensionEnabled(const char* extensionName) const override
    {
        return enabledExtensions.device.find(extensionName) != enabledExtensions.device.end();
    }
    
    bool isVulkanLayerEnabled(const char* layerName) const override
    {
        return enabledExtensions.layers.find(layerName) != enabledExtensions.layers.end();
    }

    void getEnabledVulkanInstanceExtensions(std::vector<std::string>& extensions) const override
    {
        for (const auto& ext : enabledExtensions.instance)
            extensions.push_back(ext);
    }

    void getEnabledVulkanDeviceExtensions(std::vector<std::string>& extensions) const override
    {
        for (const auto& ext : enabledExtensions.device)
            extensions.push_back(ext);
    }

    void getEnabledVulkanLayers(std::vector<std::string>& layers) const override
    {
        for (const auto& ext : enabledExtensions.layers)
            layers.push_back(ext);
    }

    bool createInstance();
    bool createWindowSurface();
    void installDebugCallback();
    bool pickPhysicalDevice();
    bool findQueueFamilies(vk::PhysicalDevice physicalDevice);
    bool createDevice();
    bool createSwapChain();
    void destroySwapChain();

    struct VulkanExtensionSet
    {
        std::unordered_set<std::string> instance;
        std::unordered_set<std::string> layers;
        std::unordered_set<std::string> device;
    };

    // minimal set of required extensions
    VulkanExtensionSet enabledExtensions = {
        // instance
        {
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME
        },
        // layers
        { },
        // device
        { },
    };

    // optional extensions
    VulkanExtensionSet optionalExtensions = {
        // instance
        { 
            VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
            VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME,
        },
        // layers
        { },
        // device
        { 
            VK_EXT_DEBUG_MARKER_EXTENSION_NAME,
            VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
            VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
            VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME,
            VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME,
            VK_EXT_MESH_SHADER_EXTENSION_NAME,
            VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME,
#if CAUSTICA_WITH_AFTERMATH
            VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME,
            VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME,
#endif
        },
    };

    std::unordered_set<std::string> m_RayTracingExtensions = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
        VK_KHR_RAY_QUERY_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_NV_CLUSTER_ACCELERATION_STRUCTURE_EXTENSION_NAME
    };

    std::string m_RendererString;

    vk::Instance m_VulkanInstance;
    vk::DebugReportCallbackEXT m_DebugReportCallback;

    vk::PhysicalDevice m_VulkanPhysicalDevice;
    int m_GraphicsQueueFamily = -1;
    int m_ComputeQueueFamily = -1;
    int m_TransferQueueFamily = -1;
    int m_PresentQueueFamily = -1;

    vk::Device m_VulkanDevice;
    vk::Queue m_GraphicsQueue;
    vk::Queue m_ComputeQueue;
    vk::Queue m_TransferQueue;
    vk::Queue m_PresentQueue;
    
    vk::SurfaceKHR m_WindowSurface;

    vk::SurfaceFormatKHR m_SwapChainFormat;
    vk::SwapchainKHR m_SwapChain;
    bool m_SwapChainMutableFormatSupported = false;

    struct SwapChainImage
    {
        vk::Image image;
        nvrhi::TextureHandle rhiHandle;
    };

    std::vector<SwapChainImage> m_SwapChainImages;
    uint32_t m_SwapChainIndex = uint32_t(-1);

    nvrhi::vulkan::DeviceHandle m_NvrhiDevice;
    nvrhi::DeviceHandle m_ValidationLayer;

    std::vector<vk::Semaphore> m_AcquireSemaphores;
    std::vector<vk::Semaphore> m_PresentSemaphores;
    uint32_t m_AcquireSemaphoreIndex = 0;

    std::queue<nvrhi::EventQueryHandle> m_FramesInFlight;
    std::vector<nvrhi::EventQueryHandle> m_QueryPool;

    bool m_BufferDeviceAddressSupported = false;

#if VK_HEADER_VERSION >= 301
    typedef vk::detail::DynamicLoader VulkanDynamicLoader;
#else
    typedef vk::DynamicLoader VulkanDynamicLoader;
#endif

    std::unique_ptr<VulkanDynamicLoader> m_dynamicLoader;
};

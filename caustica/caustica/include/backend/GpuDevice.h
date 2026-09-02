#pragma once

// Device: logical GPU. Presentation lives on GpuSurface.
//
// Create pipeline (create):
//   backend object → API instance → adapter → logical device
//   → GpuSurface (window swapchain or headless targets)
//
// Hosts pass GpuDeviceCreateDesc. Backend knobs live in
// backend/internal/DeviceParams.h and are not a second public create API.

#if CAUSTICA_WITH_DX12
#include <d3d12.h>
#endif

#if CAUSTICA_WITH_AFTERMATH
#include <backend/AftermathCrashDump.h>
#endif

#if CAUSTICA_WITH_STREAMLINE
#include <backend/StreamlineInterface.h>
#endif

#include <backend/internal/DeviceParams.h>
#include <backend/GpuSurface.h>
#include <backend/SwapChain.h>
#include <rhi/rhi.h>
#include <core/log.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace caustica
{

class Window;

struct DefaultMessageCallback : public caustica::rhi::MessageCallback
{
    static DefaultMessageCallback& getInstance();

    void message(caustica::rhi::MessageSeverity severity, const char* messageText) override;
};

struct VideoMemoryInfo
{
    uint64_t budget = 0;
    uint64_t currentUsage = 0;
    uint64_t availableForReservation = 0;
    uint64_t currentReservation = 0;
};

using AdapterInfo = caustica::rhi::AdapterDesc;

struct GpuDeviceCreateDesc
{
    caustica::rhi::GraphicsAPI api = caustica::rhi::GraphicsAPI::D3D12;
    bool headless = false;
    std::string windowTitle = "caustica";

    uint32_t backBufferWidth = 1280;
    uint32_t backBufferHeight = 720;
    uint32_t swapChainBufferCount = 0; // 0 = engine default (triple buffering)
    bool startFullscreen = false;
    bool startMaximized = false;
    bool startBorderless = false;
    bool vsyncEnabled = true;
    caustica::rhi::AdapterSelector adapter;
    bool enableDebug = false;

#if CAUSTICA_WITH_DX12
    ID3D12DeviceFactory* d3d12DeviceFactory = nullptr;
#endif
};

struct GpuDeviceCreateResult;

struct BackBufferInfo
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sampleCount = 1;
};

struct PresentRuntimeInfo
{
    bool headless = false;
    bool requestedVsync = false;
    bool activeVsync = false;
    bool windowed = true;
    bool tearingSupported = false;
    bool tearingActive = false;
    uint32_t backBufferCount = 0;
};

// Create a platform window from device/window fields. GpuDevice::create does not own windows.
[[nodiscard]] std::unique_ptr<Window> createGpuWindow(const GpuDeviceCreateDesc& desc);

class GpuDevice
{
public:
    // Logical GPU + presentation surface. Windowed mode requires an already-created Window.
    static GpuDeviceCreateResult create(const GpuDeviceCreateDesc& desc, Window* window = nullptr);

    static bool enumerateAvailableAdapters(
        caustica::rhi::GraphicsAPI api,
        std::vector<AdapterInfo>& outAdapters,
        bool enableDebug = false,
        std::string* outError = nullptr);

    [[nodiscard]] bool supportsRayTracingPipeline() const;
    [[nodiscard]] bool supportsRayQuery() const;
    [[nodiscard]] bool supportsShaderExecutionReordering() const;

    virtual bool enumerateAdapters(std::vector<AdapterInfo>& outAdapters) = 0;

    [[nodiscard]] const std::optional<AdapterInfo>& getSelectedAdapter() const;

    void setShuttingDown(bool value) { m_ShuttingDown.store(value, std::memory_order_release); }
    [[nodiscard]] bool isShuttingDown() const { return m_ShuttingDown.load(std::memory_order_acquire); }

    void seedFrameClock(double timestamp);
    void advanceFrameClock(double elapsedTime, double currentTime);

    void requestRenderUnfocused() { m_RequestedRenderUnfocused = true; }
    void clearRenderUnfocusedRequest() { m_RequestedRenderUnfocused = false; }
    [[nodiscard]] bool wantsRenderUnfocused() const { return m_RequestedRenderUnfocused; }

#if CAUSTICA_WITH_AFTERMATH
    [[nodiscard]] bool isAftermathEnabled() const { return m_DeviceParams.enableAftermath; }
#else
    [[nodiscard]] bool isAftermathEnabled() const { return false; }
#endif

    [[nodiscard]] virtual caustica::rhi::Device* getDevice() const = 0;
    [[nodiscard]] virtual const char* getRendererString() const = 0;
    [[nodiscard]] virtual caustica::rhi::GraphicsAPI getGraphicsAPI() const = 0;

    [[nodiscard]] BackBufferInfo getBackBufferInfo() const;
    [[nodiscard]] double getAverageFrameTimeSeconds() const { return m_AverageFrameTime; }
    [[nodiscard]] double getPreviousFrameTimestamp() const { return m_PreviousFrameTimestamp; }
    void setFrameTimeUpdateInterval(double seconds) { m_AverageTimeUpdateInterval = seconds; }
    [[nodiscard]] bool isHeadless() const { return m_DeviceParams.headlessDevice; }
    [[nodiscard]] bool isVsyncEnabled() const { return m_DeviceParams.vsyncEnabled; }
    [[nodiscard]] virtual PresentRuntimeInfo getPresentRuntimeInfo() const
    {
        PresentRuntimeInfo info;
        info.headless = m_DeviceParams.headlessDevice;
        info.requestedVsync = m_RequestedVSync.load(std::memory_order_relaxed);
        info.activeVsync = m_DeviceParams.vsyncEnabled;
        info.windowed = !m_DeviceParams.startFullscreen;
        info.backBufferCount = m_DeviceParams.swapChainBufferCount;
        return info;
    }
    [[nodiscard]] bool supportsExplicitDisplayScaling() const { return m_DeviceParams.supportExplicitDisplayScaling; }
    virtual void setVsyncEnabled(bool enabled)
    {
        m_RequestedVSync.store(enabled, std::memory_order_relaxed);
    }
    virtual void reportLiveObjects() {}

    [[nodiscard]] virtual bool queryVideoMemoryInfo(VideoMemoryInfo& out) const;

    [[nodiscard]] bool isD3D12() const { return getGraphicsAPI() == caustica::rhi::GraphicsAPI::D3D12; }
    [[nodiscard]] bool isVulkan() const { return getGraphicsAPI() == caustica::rhi::GraphicsAPI::VULKAN; }

    [[nodiscard]] GpuSurface* surface() const { return m_surface; }

    [[nodiscard]] uint32_t getFrameIndex() const { return m_FrameIndex; }
    [[nodiscard]] uint32_t getRenderPhaseFrameIndex() const { return m_renderPhaseFrameIndex; }
    void setRenderPhaseFrameIndex(uint32_t frameIndex) { m_renderPhaseFrameIndex = frameIndex; }
    [[nodiscard]] uint32_t getPreparedRenderFrameIndex() const { return m_preparedRenderFrameIndex; }
    void setPreparedRenderFrameIndex(uint32_t frameIndex) { m_preparedRenderFrameIndex = frameIndex; }

    virtual caustica::rhi::Texture* getCurrentBackBuffer() = 0;
    virtual caustica::rhi::Texture* getBackBuffer(uint32_t index) = 0;
    virtual uint32_t getCurrentBackBufferIndex() = 0;
    virtual uint32_t getBackBufferCount() = 0;
    caustica::rhi::Framebuffer* getCurrentFramebuffer(bool withDepth = true);
    caustica::rhi::Framebuffer* getFramebuffer(uint32_t index, bool withDepth = true);
    caustica::rhi::Texture* getDepthBuffer() const { return m_SwapChain.depthBuffer; }

    virtual void shutdown();
    virtual ~GpuDevice() = default;

    virtual bool isVulkanInstanceExtensionEnabled(const char* extensionName) const { return false; }
    virtual bool isVulkanDeviceExtensionEnabled(const char* extensionName) const { return false; }
    virtual bool isVulkanLayerEnabled(const char* layerName) const { return false; }
    virtual void getEnabledVulkanInstanceExtensions(std::vector<std::string>& extensions) const {}
    virtual void getEnabledVulkanDeviceExtensions(std::vector<std::string>& extensions) const {}
    virtual void getEnabledVulkanLayers(std::vector<std::string>& layers) const {}

#if CAUSTICA_WITH_STREAMLINE
    static StreamlineInterface& getStreamline();
#endif

protected:
    friend class GpuSurface;

    SwapChain m_SwapChain;
    DeviceCreationParameters m_DeviceParams;
    std::unique_ptr<caustica::rhi::DeviceFactory> m_DeviceFactory;
    int m_RequestedAdapterIndex = -1;
    int m_SelectedAdapterIndex = -1;

    bool m_EnableRenderDuringWindowMovement = false;
    bool m_CanPresentSwapChain = true;
    bool m_IsNvidia = false;
    std::atomic<bool> m_ShuttingDown{false};
    double m_PreviousFrameTimestamp = 0.0;
    std::atomic<bool> m_RequestedVSync{false};
    bool m_InstanceCreated = false;
    bool m_RequestedRenderUnfocused = true;

    double m_AverageFrameTime = 0.0;
    double m_AverageTimeUpdateInterval = 0.5;
    double m_FrameTimeSum = 0.0;
    int m_NumberOfAccumulatedFrames = 0;

    uint32_t m_FrameIndex = 0;
    uint32_t m_renderPhaseFrameIndex = 0;
    uint32_t m_preparedRenderFrameIndex = 0;

    std::vector<caustica::rhi::TextureHandle> m_HeadlessBackBuffers;
    uint32_t m_HeadlessBackBufferIndex = 0;

    GpuDevice();

    static GpuDevice* createBackend(caustica::rhi::GraphicsAPI api);
    bool createInstance(const InstanceParameters& params);
    bool bindPresentTarget(Window& window);
    bool createLogicalDevice(const DeviceCreationParameters& params);
    bool createHeadlessTargets(const DeviceCreationParameters& params);

    [[nodiscard]] GLFWwindow* presentGlfw() const { return m_presentGlfw; }
    [[nodiscard]] void* nativeWindowHandle() const { return m_nativeWindowHandle; }

    void beginSurfaceResize();
    void endSurfaceResize();
    void createDepthBuffer();
    bool createHeadlessBackBuffers();
    void releaseHeadlessBackBuffers();
    bool beginHeadlessFrame();
    bool presentHeadlessFrame();
    caustica::rhi::Texture* getHeadlessBackBuffer(uint32_t index);
    uint32_t getCurrentHeadlessBackBufferIndex() const;
    uint32_t getHeadlessBackBufferCount() const;

    void updateAverageFrameTime(double elapsedTime);
    virtual bool createInstanceInternal() = 0;
    virtual bool createDevice() = 0;
    virtual bool createSwapChain() = 0;
    bool validatePathTracerRequirements() const;
    virtual void destroyDeviceAndSwapChain() = 0;
    virtual void resizeSwapChain() = 0;
    virtual bool beginFrame() = 0;
    virtual bool present() = 0;
    virtual void prepareShutdown() {}

private:
    static GpuDevice* createD3D11();
    static GpuDevice* createD3D12();
    static GpuDevice* createVK();

    void attachSurface(GpuSurface* surface) { m_surface = surface; }

    GpuSurface* m_surface = nullptr;
    GLFWwindow* m_presentGlfw = nullptr;
    void* m_nativeWindowHandle = nullptr;
#if CAUSTICA_WITH_AFTERMATH
    AftermathCrashDump m_AftermathCrashDumper;
#endif
};

struct GpuDeviceCreateResult
{
    std::unique_ptr<GpuDevice> gpuDevice;
    std::unique_ptr<GpuSurface> surface;
};

} // namespace caustica

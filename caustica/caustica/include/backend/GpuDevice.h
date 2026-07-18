#pragma once

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

#if CAUSTICA_WITH_AFTERMATH
#include <backend/AftermathCrashDump.h>
#endif

#if CAUSTICA_WITH_STREAMLINE
#include <backend/StreamlineInterface.h>
#endif

#define GLFW_INCLUDE_NONE // Do not include any OpenGL headers
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif // _WIN32
#include <GLFW/glfw3native.h>
#include <rhi/nvrhi.h>
#include <core/log.h>
#include <backend/GpuFrameDriver.h>
#include <backend/SwapChain.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <atomic>

namespace caustica
{
    class Window; // Platform layer: window abstraction
    class App; // Engine layer: frame driver (friend — accesses protected state)

    struct DefaultMessageCallback : public nvrhi::IMessageCallback
    {
        static DefaultMessageCallback& getInstance();

        void message(nvrhi::MessageSeverity severity, const char* messageText) override;
    };

    struct InstanceParameters
    {
        bool enableDebugRuntime = false;
        bool enableWarningsAsErrors = false;
        bool enableGPUValidation = false; // Affects only DX12
        bool headlessDevice = false;
#if CAUSTICA_WITH_AFTERMATH
        bool enableAftermath = false;
#endif
        bool logBufferLifetime = false;
        bool enableHeapDirectlyIndexed = false; // Allows ResourceDescriptorHeap on DX12

        // Enables per-monitor DPI scale support.
        //
        // If set to true, the app will receive displayScaleChanged() events on DPI change and can read
        // the scaling factors using getDPIScaleInfo(...). The window may be resized when DPI changes if
        // DeviceCreationParameters::resizeWindowWithDisplayScale is true.
        //
        // If set to false, the app will see DPI scaling factors being 1.0 all the time, but the OS
        // may scale the contents of the window based on DPI.
        //
        // This field is located in InstanceParameters and not DeviceCreationParameters because it is needed
        // in the createInstance() function to override the glfwInit() behavior.
        bool enablePerMonitorDPI = false;

        // Severity of the information log messages from the device manager, like the device name or enabled extensions.
        caustica::Severity infoLogSeverity = caustica::Severity::Info;

#if CAUSTICA_WITH_VULKAN
        // Allows overriding the Vulkan library name with something custom, useful for Streamline
        std::string vulkanLibraryName;
        
        std::vector<std::string> requiredVulkanInstanceExtensions;
        std::vector<std::string> requiredVulkanLayers;
        std::vector<std::string> optionalVulkanInstanceExtensions;
        std::vector<std::string> optionalVulkanLayers;
#endif

#if CAUSTICA_WITH_STREAMLINE
        int streamlineAppId = 1; // default app id
        bool checkStreamlineSignature = true; // check if the streamline dlls are signed
        bool enableStreamlineLog = false;
#endif
    };

    struct DeviceCreationParameters : public InstanceParameters
    {
        bool startMaximized = false; // ignores backbuffer width/height to be monitor size
        bool startFullscreen = false;
        bool startBorderless = false;
        bool allowModeSwitch = false;
        int windowPosX = -1;            // -1 means use default placement
        int windowPosY = -1;
        uint32_t backBufferWidth = 1280;
        uint32_t backBufferHeight = 720;
        uint32_t refreshRate = 0;
        uint32_t swapChainBufferCount = 3;
        nvrhi::Format swapChainFormat = nvrhi::Format::SRGBA8_UNORM;
        uint32_t swapChainSampleCount = 1;
        uint32_t swapChainSampleQuality = 0;

        // Sets the format for the primary depth buffer. UNKNOWN means no depth buffer (legacy behavior).
        // The depth buffer is attached to every swap chain framebuffer provided to the render passes.
        nvrhi::Format depthBufferFormat = nvrhi::Format::UNKNOWN;

        uint32_t maxFramesInFlight = 2;
        bool enableNvrhiValidationLayer = false;
        bool vsyncEnabled = false;
        bool enableRayTracingExtensions = false; // for vulkan
        bool requirePathTracerFeatures = true;   // Ray tracing pipeline + ray query
        bool enableComputeQueue = false;
        bool enableCopyQueue = false;

        // Index of the adapter (DX11, DX12) or physical device (Vk) on which to initialize the device.
        // Negative values mean automatic detection.
        // The order of indices matches that returned by GpuDevice::enumerateAdapters.
        int adapterIndex = -1;

        // Set this to true if the application implements UI scaling for DPI explicitly instead of relying
        // on ImGUI's DisplayFramebufferScale. This produces crisp text and lines at any scale
        // but requires considerable changes to applications that rely on the old behavior:
        // all UI sizes and offsets need to be computed as multiples of some scaled parameter,
        // such as ImGui::GetFontSize(). Note that the ImGUI style is automatically reset and scaled in 
        // ImGui_Renderer::displayScaleChanged(...).
        //
        // See ImGUI FAQ for more info:
        //   https://github.com/ocornut/imgui/blob/master/docs/FAQ.md#q-how-should-i-handle-dpi-in-my-application
        bool supportExplicitDisplayScaling = false;

        // Enables automatic resizing of the application window according to the DPI scaling of the monitor
        // that it is located on. When set to true and the app launches on a monitor with >100% scale, 
        // the initial window size will be larger than specified in 'backBufferWidth' and 'backBufferHeight' parameters.
        bool resizeWindowWithDisplayScale = false;

        nvrhi::IMessageCallback *messageCallback = nullptr;

#if CAUSTICA_WITH_DX11 || CAUSTICA_WITH_DX12
        DXGI_USAGE swapChainUsage = DXGI_USAGE_SHADER_INPUT | DXGI_USAGE_RENDER_TARGET_OUTPUT;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;
#endif

#if CAUSTICA_WITH_DX12
        // Optional Agility SDK device factory. This is useful for DLL/plugin hosts
        // (for example Python extension modules) where the main executable cannot
        // export D3D12SDKVersion/D3D12SDKPath like a normal standalone app.
        ID3D12DeviceFactory* d3d12DeviceFactory = nullptr;
#endif

#if CAUSTICA_WITH_VULKAN
        std::vector<std::string> requiredVulkanDeviceExtensions;
        std::vector<std::string> optionalVulkanDeviceExtensions;
        std::vector<size_t> ignoredVulkanValidationMessageLocations = {
            // Ignore the warnings like "the storage image descriptor [...] is accessed by a OpTypeImage that has
            //   a Format operand ... which doesn't match the VkImageView ..." -- even when the GPU supports
            // storage without format, which all modern GPUs do, there is no good way to enable it in the shaders.
            0x13365b2
        };
        std::function<void(VkDeviceCreateInfo&)> deviceCreateInfoCallback;

        // This pointer specifies an optional structure to be put at the end of the chain for 'vkGetPhysicalDeviceFeatures2' call.
        // The structure may also be a chain, and must be alive during the device initialization process.
        // The elements of this structure will be populated before 'deviceCreateInfoCallback' is called,
        // thereby allowing applications to determine if certain features may be enabled on the device.
        void* physicalDeviceFeatures2Extensions = nullptr;
#endif
    };

    struct VideoMemoryInfo
    {
        uint64_t budget = 0;
        uint64_t currentUsage = 0;
        uint64_t availableForReservation = 0;
        uint64_t currentReservation = 0;
    };

    struct AdapterInfo
    {
        typedef std::array<uint8_t, 16> UUID;
        typedef std::array<uint8_t, 8> LUID;

        std::string name;
        uint32_t vendorID = 0;
        uint32_t deviceID = 0;
        uint64_t dedicatedVideoMemory = 0;

        std::optional<UUID> uuid;
        std::optional<LUID> luid;

#if CAUSTICA_WITH_DX11 || CAUSTICA_WITH_DX12
        nvrhi::RefCountPtr<IDXGIAdapter> dxgiAdapter;
#endif
#if CAUSTICA_WITH_VULKAN
        VkPhysicalDevice vkPhysicalDevice = nullptr;
#endif
    };

    struct GpuDeviceCreateDesc
    {
        nvrhi::GraphicsAPI api = nvrhi::GraphicsAPI::D3D12;
        bool headless = false;
        std::string windowTitle = "caustica";

        uint32_t backBufferWidth = 1280;
        uint32_t backBufferHeight = 720;
        uint32_t swapChainBufferCount = 0; // 0 = engine default (triple buffering)
        bool startFullscreen = false;
        bool startBorderless = false;
        bool vsyncEnabled = true;
        int adapterIndex = -1;
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

    class GpuDevice
    {
        friend class App;

    public:
        // Application entry: creates GpuDevice (+ optional Window/swap chain or headless buffers).
        static GpuDeviceCreateResult createInitialized(const GpuDeviceCreateDesc& desc);

        [[nodiscard]] bool supportsRayTracingPipeline() const;
        [[nodiscard]] bool supportsRayQuery() const;
        [[nodiscard]] bool supportsShaderExecutionReordering() const;

        // Initializes device-independent objects (DXGI factory, Vulkan instnace).
        // Calling createInstance() is required before enumerateAdapters(), but optional if you don't use enumerateAdapters().
        // Note: if you call createInstance before device initialization, the values in InstanceParameters must match those
        // in DeviceCreationParameters passed to the device call.
        bool createInstance(const InstanceParameters& params);

        // Enumerates adapters or physical devices present in the system.
        // Note: a call to createInstance() or createInitialized() is required before enumerateAdapters().
        virtual bool enumerateAdapters(std::vector<AdapterInfo>& outAdapters) = 0;

        void setFrameDriver(IGpuFrameDriver* driver) { m_frameDriver = driver; }
        [[nodiscard]] IGpuFrameDriver* getFrameDriver() const { return m_frameDriver; }
        void waitForRenderThreadIdle();

        // Set when the app is closing so in-flight render work can skip RTPSO rebuilds
        // and other long stalls that would freeze window close.
        void setShuttingDown(bool value) { m_ShuttingDown.store(value, std::memory_order_release); }
        [[nodiscard]] bool isShuttingDown() const { return m_ShuttingDown.load(std::memory_order_acquire); }

        // returns the size of the window in screen coordinates
        void getWindowDimensions(int& width, int& height);
        // returns the screen coordinate to pixel coordinate scale factor
        void getDPIScaleInfo(float& x, float& y) const;

    protected:
        // useful for apps that require 2 frames worth of simulation data before first render
        // apps should extend the GpuDevice classes, and constructor initialized this to true to opt in to the behavior
        bool m_SkipRenderOnFirstFrame = false;

        SwapChain m_SwapChain;                     // Backend layer: swapchain state
        DeviceCreationParameters m_DeviceParams;
        GLFWwindow* m_Window = nullptr;       // Borrowed GLFW handle from GlfwWindow
        Window* m_WindowPtr = nullptr;        // Platform layer window (owns GLFW lifetime)

        bool m_EnableRenderDuringWindowMovement = false;
        bool m_CanPresentSwapChain = true;    // false when the window is minimized
        // set to true if running on NV GPU
        bool m_IsNvidia = false;
        IGpuFrameDriver* m_frameDriver = nullptr;
        std::atomic<bool> m_ShuttingDown{false};
        // timestamp in seconds for the previous frame
        double m_PreviousFrameTimestamp = 0.0;
        // current DPI scale info (updated when window moves)
        float m_DPIScaleFactorX = 1.f;
        float m_DPIScaleFactorY = 1.f;
        float m_PrevDPIScaleFactorX = 0.f;
        float m_PrevDPIScaleFactorY = 0.f;
        bool m_RequestedVSync = false;
        bool m_InstanceCreated = false;
        bool m_RequestedRenderUnfocused = true;

        double m_AverageFrameTime = 0.0;
        double m_AverageTimeUpdateInterval = 0.5;
        double m_FrameTimeSum = 0.0;
        int m_NumberOfAccumulatedFrames = 0;

        uint32_t m_FrameIndex = 0;
        // Frame index for the render phase currently executing (async-safe; set in executeRenderPhase).
        uint32_t m_renderPhaseFrameIndex = 0;
        // Frame index for the upcoming render (set on the main thread before onPrepareRenderScene).
        uint32_t m_preparedRenderFrameIndex = 0;

        std::vector<nvrhi::TextureHandle> m_HeadlessBackBuffers;
        uint32_t m_HeadlessBackBufferIndex = 0;

        GpuDevice();

        static GpuDevice* create(nvrhi::GraphicsAPI api);
        bool initializeGraphicsDevice(const DeviceCreationParameters& params);
        bool initializeWindowSwapChain(class Window* window);
        bool initializeHeadlessGraphics(const DeviceCreationParameters& params);

        void updateWindowSize();
        // UI-thread probe: true when resize / minimize / vsync actually needs a
        // render-thread sync. Avoids per-frame dispatchAndWait behind path-trace.
        [[nodiscard]] bool needsWindowSizeSync() const;
        void backBufferResizing();
        void backBufferResized();
        void createDepthBuffer();
        bool createHeadlessBackBuffers();
        void releaseHeadlessBackBuffers();
        bool beginHeadlessFrame();
        bool presentHeadlessFrame();
        nvrhi::ITexture* getHeadlessBackBuffer(uint32_t index);
        uint32_t getCurrentHeadlessBackBufferIndex() const;
        uint32_t getHeadlessBackBufferCount() const;

        void updateAverageFrameTime(double elapsedTime);
        // device-specific methods
        virtual bool createInstanceInternal() = 0;
        virtual bool createDevice() = 0;
        virtual bool createSwapChain() = 0;
        bool validatePathTracerRequirements() const;
        virtual void destroyDeviceAndSwapChain() = 0;
        virtual void resizeSwapChain() = 0;
        virtual bool beginFrame() = 0;
        virtual bool present() = 0;
        virtual void prepareShutdown() {}

    public:
        [[nodiscard]] virtual nvrhi::IDevice *getDevice() const = 0;
        [[nodiscard]] virtual const char *getRendererString() const = 0;
        [[nodiscard]] virtual nvrhi::GraphicsAPI getGraphicsAPI() const = 0;

        [[nodiscard]] BackBufferInfo getBackBufferInfo() const;
        [[nodiscard]] double getAverageFrameTimeSeconds() const { return m_AverageFrameTime; }
        [[nodiscard]] double getPreviousFrameTimestamp() const { return m_PreviousFrameTimestamp; }
        void setFrameTimeUpdateInterval(double seconds) { m_AverageTimeUpdateInterval = seconds; }
        [[nodiscard]] bool isHeadless() const { return m_DeviceParams.headlessDevice; }
        [[nodiscard]] bool isVsyncEnabled() const { return m_DeviceParams.vsyncEnabled; }
        [[nodiscard]] bool supportsExplicitDisplayScaling() const { return m_DeviceParams.supportExplicitDisplayScaling; }
        virtual void setVsyncEnabled(bool enabled) { m_RequestedVSync = enabled; /* will be processed later */ }
        virtual void reportLiveObjects() {}

        [[nodiscard]] virtual bool queryVideoMemoryInfo(VideoMemoryInfo& out) const;

        [[nodiscard]] bool isD3D12() const { return getGraphicsAPI() == nvrhi::GraphicsAPI::D3D12; }
        [[nodiscard]] bool isVulkan() const { return getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN; }

        [[nodiscard]] GLFWwindow* getWindow() const { return m_Window; }
        [[nodiscard]] Window* getPlatformWindow() const { return m_WindowPtr; }

        // When window is owned externally (new 4-layer Window), call this
        // before shutdown() to prevent double-free of the GLFW window.
        void releaseWindowOwnership() { m_Window = nullptr; }

        [[nodiscard]] uint32_t getFrameIndex() const { return m_FrameIndex; }
        [[nodiscard]] uint32_t getRenderPhaseFrameIndex() const { return m_renderPhaseFrameIndex; }
        void setRenderPhaseFrameIndex(uint32_t frameIndex) { m_renderPhaseFrameIndex = frameIndex; }
        [[nodiscard]] uint32_t getPreparedRenderFrameIndex() const { return m_preparedRenderFrameIndex; }
        void setPreparedRenderFrameIndex(uint32_t frameIndex) { m_preparedRenderFrameIndex = frameIndex; }

        virtual nvrhi::ITexture* getCurrentBackBuffer() = 0;
        virtual nvrhi::ITexture* getBackBuffer(uint32_t index) = 0;
        virtual uint32_t getCurrentBackBufferIndex() = 0;
        virtual uint32_t getBackBufferCount() = 0;
        nvrhi::IFramebuffer* getCurrentFramebuffer(bool withDepth = true);
        nvrhi::IFramebuffer* getFramebuffer(uint32_t index, bool withDepth = true);
        nvrhi::ITexture* getDepthBuffer() const { return m_SwapChain.depthBuffer; }

        virtual void shutdown();
        virtual ~GpuDevice() = default;

        void setWindowTitle(const char* title);
        void setInformativeWindowTitle(const char* applicationName, bool includeFramerate = true, const char* extraInfo = nullptr);
        const char* getWindowTitle();

        virtual bool isVulkanInstanceExtensionEnabled(const char* extensionName) const { return false; }
        virtual bool isVulkanDeviceExtensionEnabled(const char* extensionName) const { return false; }
        virtual bool isVulkanLayerEnabled(const char* layerName) const { return false; }
        virtual void getEnabledVulkanInstanceExtensions(std::vector<std::string>& extensions) const { }
        virtual void getEnabledVulkanDeviceExtensions(std::vector<std::string>& extensions) const { }
        virtual void getEnabledVulkanLayers(std::vector<std::string>& layers) const { }

        // getFrameIndex cannot be used inside of these callbacks, hence the additional passing of frameID.
        // Frame lifecycle callbacks; typically wired by the IGpuFrameDriver owner (engine Application).
        struct PipelineCallbacks {
            std::function<void(GpuDevice&, uint32_t)> beforeFrame = nullptr;
            std::function<void(GpuDevice&, uint32_t)> beforeAnimate = nullptr;
            std::function<void(GpuDevice&, uint32_t)> afterAnimate = nullptr;
            std::function<void(GpuDevice&, uint32_t)> beforeRender = nullptr;
            std::function<void(GpuDevice&, uint32_t)> afterRender = nullptr;
            std::function<void(GpuDevice&, uint32_t)> beforePresent = nullptr;
            std::function<void(GpuDevice&, uint32_t)> afterPresent = nullptr;
        } m_callbacks;

#if CAUSTICA_WITH_STREAMLINE
        static StreamlineInterface& getStreamline();
#endif

    private:
        static GpuDevice* createD3D11();
        static GpuDevice* createD3D12();
        static GpuDevice* createVK();

        std::string m_WindowTitle;
#if CAUSTICA_WITH_AFTERMATH
        AftermathCrashDump m_AftermathCrashDumper;
#endif
    };

    struct GpuDeviceCreateResult
    {
        std::unique_ptr<GpuDevice> gpuDevice;
        std::unique_ptr<Window> window;
    };

}

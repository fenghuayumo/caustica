#include <backend/GpuDevice.h>
#include <platform/window.h>
#include <platform/glfw_window.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3native.h>
#include <core/log.h>

#include <algorithm>

#if CAUSTICA_WITH_DX11
#include <d3d11.h>
#endif

#if CAUSTICA_WITH_DX12
#include <d3d12.h>
#endif

#if CAUSTICA_WITH_STREAMLINE
#include <StreamlineIntegration.h>
#endif

#ifdef _WINDOWS
#include <ShellScalingApi.h>
#pragma comment(lib, "shcore.lib")
#endif

#if defined(_WINDOWS) && CAUSTICA_FORCE_DISCRETE_GPU
extern "C"
{
    // Declaring this symbol makes the OS run the app on the discrete GPU on NVIDIA Optimus laptops by default
    __declspec(dllexport) DWORD NvOptimusEnablement = 1;
    // Same as above, for laptops with AMD GPUs
    __declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 1;
}
#endif

using namespace caustica;

bool GpuDevice::createInstance(const InstanceParameters& params)
{
    if (m_InstanceCreated)
        return true;


    static_cast<InstanceParameters&>(m_DeviceParams) = params;

    if (!params.headlessDevice)
    {
#ifdef _WINDOWS
        if (!params.enablePerMonitorDPI)
        {
            // glfwInit enables the maximum supported level of DPI awareness unconditionally.
            // If the app doesn't need it, we have to call this function before glfwInit to override that behavior.
            SetProcessDpiAwareness(PROCESS_DPI_UNAWARE);
        }
#endif

        if (!glfwInit())
            return false;
    }

#if CAUSTICA_WITH_AFTERMATH
    if (params.enableAftermath)
    {
        m_AftermathCrashDumper.enableCrashDumpTracking();
    }
#endif

    m_InstanceCreated = createInstanceInternal();
    return m_InstanceCreated;
}

bool GpuDevice::createHeadlessTargets(const DeviceCreationParameters& params)
{
    return createLogicalDevice(params);
}

bool GpuDevice::createLogicalDevice(const DeviceCreationParameters& params)
{
    m_DeviceParams = params;
    m_RequestedVSync = params.vsyncEnabled;

    if (!createInstance(m_DeviceParams))
        return false;

    m_DeviceFactory = std::make_unique<caustica::rhi::DeviceFactory>(
        [this](std::vector<AdapterInfo>& adapters, std::string& errorText) {
            if (enumerateAdapters(adapters))
                return true;
            errorText = "backend adapter enumeration failed";
            return false;
        },
        [this](int index, std::string& errorText) {
            m_RequestedAdapterIndex = index;
            m_SelectedAdapterIndex = -1;
            if (createDevice())
                return true;
            errorText = "backend device creation failed";
            return false;
        },
        [this]() { return m_SelectedAdapterIndex; });

    const caustica::rhi::DeviceFactoryCreateResult createResult =
        m_DeviceFactory->createDevice(m_DeviceParams.adapter);
    if (!createResult)
    {
        caustica::error("GPU device creation failed: %s", createResult.error.c_str());
        return false;
    }

    caustica::message(
        m_DeviceParams.infoLogSeverity,
        "Selected GPU adapter [%u]: %s (%s)",
        createResult.adapter->index,
        createResult.adapter->name.c_str(),
        caustica::rhi::adapterTypeToString(createResult.adapter->type));

    if (m_DeviceParams.requirePathTracerFeatures && !validatePathTracerRequirements())
        return false;

    return true;
}

const std::optional<AdapterInfo>& GpuDevice::getSelectedAdapter() const
{
    static const std::optional<AdapterInfo> empty;
    return m_DeviceFactory ? m_DeviceFactory->selectedAdapter() : empty;
}

bool GpuDevice::enumerateAvailableAdapters(
    caustica::rhi::GraphicsAPI api,
    std::vector<AdapterInfo>& outAdapters,
    bool enableDebug,
    std::string* outError)
{
    outAdapters.clear();
    std::unique_ptr<GpuDevice> gpuDevice(GpuDevice::createBackend(api));
    if (!gpuDevice)
    {
        if (outError)
            *outError = "requested graphics backend is not available in this build";
        return false;
    }

    InstanceParameters params;
    params.enableDebugRuntime = enableDebug;
    params.headlessDevice = true;
#if CAUSTICA_WITH_STREAMLINE
    // Adapter discovery must not initialize or shut down process-global
    // Streamline state owned by an active renderer.
    params.enableStreamline = false;
#endif
    if (!gpuDevice->createInstance(params))
    {
        if (outError)
            *outError = "failed to create the graphics API instance";
        gpuDevice->shutdown();
        return false;
    }

    const bool success = gpuDevice->enumerateAdapters(outAdapters);
    if (!success && outError)
        *outError = "failed to enumerate GPU adapters";
    gpuDevice->shutdown();
    return success;
}

bool GpuDevice::bindPresentTarget(Window& window)
{
    if (!window.hasInitialised())
    {
        caustica::error("GpuDevice::bindPresentTarget: Window must be created first");
        return false;
    }

    m_DeviceParams.headlessDevice = false;

    GLFWwindow* glfwHandle = nativeGlfwWindow(&window);
    if (!glfwHandle)
    {
        caustica::error("GpuDevice::bindPresentTarget: Window has no GLFW handle");
        return false;
    }

    m_presentGlfw = glfwHandle;
    m_nativeWindowHandle = window.getNativeHandle();
    window.setRenderDuringMove(m_EnableRenderDuringWindowMovement);
    return true;
}

void GpuDevice::beginSurfaceResize()
{
    m_SwapChain.framebuffers.clear();
    m_SwapChain.framebuffersWithDepth.clear();
}

void GpuDevice::endSurfaceResize()
{
    const uint32_t backBufferCount = getBackBufferCount();
    std::vector<caustica::rhi::Texture*> backBuffers;
    backBuffers.reserve(backBufferCount);
    for (uint32_t index = 0; index < backBufferCount; ++index)
    {
        caustica::rhi::Texture* backBuffer = getBackBuffer(index);
        if (backBuffer == nullptr)
        {
            caustica::error("Back buffer resize produced a null buffer at index %u", index);
            m_CanPresentSwapChain = false;
            m_SwapChain.framebuffers.clear();
            m_SwapChain.framebuffersWithDepth.clear();
            return;
        }
        backBuffers.push_back(backBuffer);
    }

    createDepthBuffer();

    m_SwapChain.framebuffers.resize(backBufferCount);
    m_SwapChain.framebuffersWithDepth.resize(backBufferCount);
    for (uint32_t index = 0; index < backBufferCount; index++)
    {
        caustica::rhi::FramebufferDesc framebufferDesc = caustica::rhi::FramebufferDesc()
            .addColorAttachment(backBuffers[index]);

        m_SwapChain.framebuffers[index] = getDevice()->createFramebuffer(framebufferDesc);

        if (m_SwapChain.depthBuffer)
        {
            framebufferDesc.setDepthAttachment(m_SwapChain.depthBuffer);
            m_SwapChain.framebuffersWithDepth[index] = getDevice()->createFramebuffer(framebufferDesc);
        }
        else
        {
            m_SwapChain.framebuffersWithDepth[index] = m_SwapChain.framebuffers[index];
        }
    }
}

void GpuDevice::createDepthBuffer()
{
    m_SwapChain.depthBuffer = nullptr;

    if (m_DeviceParams.depthBufferFormat == caustica::rhi::Format::UNKNOWN)
        return;

    caustica::rhi::TextureDesc textureDesc = caustica::rhi::TextureDesc()
        .setDebugName("Depth Buffer")
        .setWidth(m_DeviceParams.backBufferWidth)
        .setHeight(m_DeviceParams.backBufferHeight)
        .setFormat(m_DeviceParams.depthBufferFormat)
        .setDimension(m_DeviceParams.swapChainSampleCount > 1
            ? caustica::rhi::TextureDimension::Texture2DMS
            : caustica::rhi::TextureDimension::Texture2D)
        .setSampleCount(m_DeviceParams.swapChainSampleCount)
        .setSampleQuality(m_DeviceParams.swapChainSampleQuality)
        .setIsTypeless(true)
        .setIsRenderTarget(true)
        .enableAutomaticStateTracking(caustica::rhi::ResourceStates::DepthWrite);

    m_SwapChain.depthBuffer = getDevice()->createTexture(textureDesc);
}

bool GpuDevice::createHeadlessBackBuffers()
{
    releaseHeadlessBackBuffers();

    if (!getDevice())
        return false;

    if (m_DeviceParams.backBufferWidth == 0 || m_DeviceParams.backBufferHeight == 0)
    {
        caustica::error("Cannot create headless back buffers with a zero-sized extent.");
        return false;
    }

    uint32_t backBufferCount = std::max(1u, m_DeviceParams.swapChainBufferCount);
    m_HeadlessBackBuffers.reserve(backBufferCount);

    for (uint32_t index = 0; index < backBufferCount; ++index)
    {
        caustica::rhi::TextureDesc textureDesc = caustica::rhi::TextureDesc()
            .setDebugName("Headless Back Buffer")
            .setWidth(m_DeviceParams.backBufferWidth)
            .setHeight(m_DeviceParams.backBufferHeight)
            .setFormat(m_DeviceParams.swapChainFormat)
            .setDimension(m_DeviceParams.swapChainSampleCount > 1
                ? caustica::rhi::TextureDimension::Texture2DMS
                : caustica::rhi::TextureDimension::Texture2D)
            .setSampleCount(m_DeviceParams.swapChainSampleCount)
            .setSampleQuality(m_DeviceParams.swapChainSampleQuality)
            .setIsRenderTarget(true)
            .setInitialState(caustica::rhi::ResourceStates::RenderTarget)
            .setKeepInitialState(true);

        caustica::rhi::TextureHandle texture = getDevice()->createTexture(textureDesc);
        if (!texture)
        {
            caustica::error("Failed to create headless back buffer %u.", index);
            releaseHeadlessBackBuffers();
            return false;
        }

        m_HeadlessBackBuffers.push_back(texture);
    }

    m_HeadlessBackBufferIndex = 0;
    return true;
}

void GpuDevice::releaseHeadlessBackBuffers()
{
    m_HeadlessBackBuffers.clear();
    m_HeadlessBackBufferIndex = 0;
}

bool GpuDevice::beginHeadlessFrame()
{
    return !m_HeadlessBackBuffers.empty();
}

bool GpuDevice::presentHeadlessFrame()
{
    if (m_HeadlessBackBuffers.empty())
        return false;

    m_HeadlessBackBufferIndex = (m_HeadlessBackBufferIndex + 1) % uint32_t(m_HeadlessBackBuffers.size());
    return true;
}

caustica::rhi::Texture* GpuDevice::getHeadlessBackBuffer(uint32_t index)
{
    if (index < m_HeadlessBackBuffers.size())
        return m_HeadlessBackBuffers[index];

    return nullptr;
}

uint32_t GpuDevice::getCurrentHeadlessBackBufferIndex() const
{
    return m_HeadlessBackBufferIndex;
}

uint32_t GpuDevice::getHeadlessBackBufferCount() const
{
    return uint32_t(m_HeadlessBackBuffers.size());
}

void GpuDevice::seedFrameClock(double timestamp)
{
    m_PreviousFrameTimestamp = timestamp;
}

void GpuDevice::advanceFrameClock(double elapsedTime, double currentTime)
{
    updateAverageFrameTime(elapsedTime);
    m_PreviousFrameTimestamp = currentTime;
    ++m_FrameIndex;
}

void GpuDevice::updateAverageFrameTime(double elapsedTime)
{
    if (elapsedTime <= 0.0)
        return;

    // Seed immediately so UI/window title have a value before the first 0.5s batch.
    if (m_AverageFrameTime <= 0.0)
        m_AverageFrameTime = elapsedTime;

    m_FrameTimeSum += elapsedTime;
    m_NumberOfAccumulatedFrames += 1;

    if (m_FrameTimeSum > m_AverageTimeUpdateInterval && m_NumberOfAccumulatedFrames > 0)
    {
        m_AverageFrameTime = m_FrameTimeSum / double(m_NumberOfAccumulatedFrames);
        m_NumberOfAccumulatedFrames = 0;
        m_FrameTimeSum = 0.0;
    }
}

BackBufferInfo GpuDevice::getBackBufferInfo() const
{
    return BackBufferInfo{
        m_DeviceParams.backBufferWidth,
        m_DeviceParams.backBufferHeight,
        m_DeviceParams.swapChainSampleCount
    };
}

bool GpuDevice::validatePathTracerRequirements() const
{
    if (!supportsRayTracingPipeline())
    {
        caustica::fatal("The graphics device does not support Ray Tracing Pipelines");
        return false;
    }

    if (!supportsRayQuery())
    {
        caustica::fatal("The graphics device does not support Ray Queries");
        return false;
    }

    return true;
}

bool GpuDevice::supportsRayTracingPipeline() const
{
    caustica::rhi::Device* device = getDevice();
    return device && device->queryFeatureSupport(caustica::rhi::Feature::RayTracingPipeline);
}

bool GpuDevice::supportsRayQuery() const
{
    caustica::rhi::Device* device = getDevice();
    return device && device->queryFeatureSupport(caustica::rhi::Feature::RayQuery);
}

bool GpuDevice::supportsShaderExecutionReordering() const
{
    caustica::rhi::Device* device = getDevice();
    return device
        && device->getGraphicsAPI() == caustica::rhi::GraphicsAPI::D3D12
        && device->queryFeatureSupport(caustica::rhi::Feature::ShaderExecutionReordering);
}

bool GpuDevice::queryVideoMemoryInfo(VideoMemoryInfo& /*out*/) const
{
    return false;
}

caustica::GpuDevice::GpuDevice()
#if CAUSTICA_WITH_AFTERMATH
    : m_AftermathCrashDumper(*this)
#endif
{
}

void GpuDevice::shutdown()
{
#if CAUSTICA_WITH_STREAMLINE
    // Shut down Streamline before destroying swap chain and device.
    if (m_DeviceParams.enableStreamline)
        StreamlineIntegration::Get().shutdown();
#endif

    prepareShutdown();

    m_SwapChain.framebuffers.clear();
    m_SwapChain.framebuffersWithDepth.clear();
    m_SwapChain.depthBuffer = nullptr;
    releaseHeadlessBackBuffers();

    destroyDeviceAndSwapChain();

    m_presentGlfw = nullptr;
    m_nativeWindowHandle = nullptr;
    m_surface = nullptr;

    m_InstanceCreated = false;
}

caustica::rhi::Framebuffer* caustica::GpuDevice::getCurrentFramebuffer(bool withDepth)
{
    return getFramebuffer(getCurrentBackBufferIndex(), withDepth);
}

caustica::rhi::Framebuffer* caustica::GpuDevice::getFramebuffer(uint32_t index, bool withDepth)
{
    if (withDepth)
    {
        if (index < m_SwapChain.framebuffersWithDepth.size())
            return m_SwapChain.framebuffersWithDepth[index];
    }
    else
    {
        if (index < m_SwapChain.framebuffers.size())
            return m_SwapChain.framebuffers[index];
    }

    return nullptr;
}

caustica::GpuDevice* caustica::GpuDevice::createBackend(caustica::rhi::GraphicsAPI api)
{
    switch (api)
    {
#if CAUSTICA_WITH_DX11
    case caustica::rhi::GraphicsAPI::D3D11:
        return createD3D11();
#endif
#if CAUSTICA_WITH_DX12
    case caustica::rhi::GraphicsAPI::D3D12:
        return createD3D12();
#endif
#if CAUSTICA_WITH_VULKAN
    case caustica::rhi::GraphicsAPI::VULKAN:
        return createVK();
#endif
    default:
        caustica::error("GpuDevice::createBackend: Unsupported Graphics API (%d)", api);
        return nullptr;
    }
}

DefaultMessageCallback& DefaultMessageCallback::getInstance()
{
    static DefaultMessageCallback Instance;
    return Instance;
}

void DefaultMessageCallback::message(caustica::rhi::MessageSeverity severity, const char* messageText)
{
    caustica::Severity logSeverity = caustica::Severity::Info;
    switch (severity)
    {
    case caustica::rhi::MessageSeverity::Info:
        logSeverity = caustica::Severity::Info;
        break;
    case caustica::rhi::MessageSeverity::Warning:
        logSeverity = caustica::Severity::Warning;
        break;
    case caustica::rhi::MessageSeverity::Error:
        logSeverity = caustica::Severity::Error;
        break;
    case caustica::rhi::MessageSeverity::Fatal:
        logSeverity = caustica::Severity::Fatal;
        break;
    }
    
    caustica::message(logSeverity, "%s", messageText);
}

#if CAUSTICA_WITH_STREAMLINE
StreamlineInterface& GpuDevice::getStreamline()
{
    // StreamlineIntegration doesn't support instances
    return StreamlineIntegration::Get();
}
#endif

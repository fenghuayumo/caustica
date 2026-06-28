#include <backend/GpuDevice.h>
#include <platform/window.h>
#include <platform/glfw_window.h>
#include <math/math.h>
#include <core/log.h>
#include <rhi/utils.h>

#include <cstdio>
#include <algorithm>
#include <iomanip>
#include <sstream>

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

// The joystick interface in glfw is not per-window like the keys, mouse, etc. The joystick callbacks
// don't take a window arg. So glfw's model is a global joystick shared by all windows. Hence, the equivalent 
// is a singleton class that all GpuDevice instances can use.

static void ErrorCallback_GLFW(int error, const char *description)
{
    fprintf(stderr, "GLFW error: %s\n", description);
    exit(1);
}

static const struct
{
    nvrhi::Format format;
    uint32_t redBits;
    uint32_t greenBits;
    uint32_t blueBits;
    uint32_t alphaBits;
    uint32_t depthBits;
    uint32_t stencilBits;
} formatInfo[] = {
    { nvrhi::Format::UNKNOWN,            0,  0,  0,  0,  0,  0, },
    { nvrhi::Format::R8_UINT,            8,  0,  0,  0,  0,  0, },
    { nvrhi::Format::RG8_UINT,           8,  8,  0,  0,  0,  0, },
    { nvrhi::Format::RG8_UNORM,          8,  8,  0,  0,  0,  0, },
    { nvrhi::Format::R16_UINT,          16,  0,  0,  0,  0,  0, },
    { nvrhi::Format::R16_UNORM,         16,  0,  0,  0,  0,  0, },
    { nvrhi::Format::R16_FLOAT,         16,  0,  0,  0,  0,  0, },
    { nvrhi::Format::RGBA8_UNORM,        8,  8,  8,  8,  0,  0, },
    { nvrhi::Format::RGBA8_SNORM,        8,  8,  8,  8,  0,  0, },
    { nvrhi::Format::BGRA8_UNORM,        8,  8,  8,  8,  0,  0, },
    { nvrhi::Format::SRGBA8_UNORM,       8,  8,  8,  8,  0,  0, },
    { nvrhi::Format::SBGRA8_UNORM,       8,  8,  8,  8,  0,  0, },
    { nvrhi::Format::R10G10B10A2_UNORM, 10, 10, 10,  2,  0,  0, },
    { nvrhi::Format::R11G11B10_FLOAT,   11, 11, 10,  0,  0,  0, },
    { nvrhi::Format::RG16_UINT,         16, 16,  0,  0,  0,  0, },
    { nvrhi::Format::RG16_FLOAT,        16, 16,  0,  0,  0,  0, },
    { nvrhi::Format::R32_UINT,          32,  0,  0,  0,  0,  0, },
    { nvrhi::Format::R32_FLOAT,         32,  0,  0,  0,  0,  0, },
    { nvrhi::Format::RGBA16_FLOAT,      16, 16, 16, 16,  0,  0, },
    { nvrhi::Format::RGBA16_UNORM,      16, 16, 16, 16,  0,  0, },
    { nvrhi::Format::RGBA16_SNORM,      16, 16, 16, 16,  0,  0, },
    { nvrhi::Format::RG32_UINT,         32, 32,  0,  0,  0,  0, },
    { nvrhi::Format::RG32_FLOAT,        32, 32,  0,  0,  0,  0, },
    { nvrhi::Format::RGB32_UINT,        32, 32, 32,  0,  0,  0, },
    { nvrhi::Format::RGB32_FLOAT,       32, 32, 32,  0,  0,  0, },
    { nvrhi::Format::RGBA32_UINT,       32, 32, 32, 32,  0,  0, },
    { nvrhi::Format::RGBA32_FLOAT,      32, 32, 32, 32,  0,  0, },
};

bool GpuDevice::CreateInstance(const InstanceParameters& params)
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
        m_AftermathCrashDumper.EnableCrashDumpTracking();
    }
#endif

    m_InstanceCreated = CreateInstanceInternal();
    return m_InstanceCreated;
}

bool GpuDevice::CreateHeadlessDevice(const DeviceCreationParameters& params)
{
    m_DeviceParams = params;
    m_DeviceParams.headlessDevice = true;

    if (!CreateInstance(m_DeviceParams))
        return false;

    if (!CreateDevice())
        return false;

    if (!CreateHeadlessBackBuffers())
        return false;

    BackBufferResized();

    return true;
}

bool GpuDevice::CreateDeviceAndSwapChain(const DeviceCreationParameters& params, Window* window)
{
    if (!window || !window->hasInitialised())
    {
        caustica::error("CreateDeviceAndSwapChain: Window must be created first");
        return false;
    }

    m_DeviceParams           = params;
    m_DeviceParams.headlessDevice = false;
    m_RequestedVSync         = params.vsyncEnabled;

    // Must create the GPU instance before creating the device.
    // CreateInstance is idempotent (checks m_InstanceCreated internally).
    if (!CreateInstance(m_DeviceParams))
    {
        caustica::error("CreateDeviceAndSwapChain: Failed to create GPU instance");
        return false;
    }

    // Get the GLFW window handle for GLFW API calls.
    // IMPORTANT: getNativeHandle() returns the platform HWND (Win32),
    // NOT a GLFWwindow*. Use GlfwWindow::glfwWindow() for GLFW API.
    GlfwWindow* glfwWin = dynamic_cast<GlfwWindow*>(window);
    if (!glfwWin)
    {
        caustica::error("CreateDeviceAndSwapChain: Window is not a GlfwWindow");
        return false;
    }

    GLFWwindow* glfwHandle = glfwWin->glfwWindow();
    if (!glfwHandle)
    {
        caustica::error("CreateDeviceAndSwapChain: Window has no GLFW handle");
        return false;
    }

    // Store both the Window* (new path) and GLFWwindow* (for legacy code)
    m_WindowPtr = window;
    m_Window = glfwHandle;  // Actual GLFWwindow*, NOT native handle
    m_WindowTitle = window->getTitle();

    // GlfwWindow already owns ALL GLFW callbacks (window events, DPI tracking).
    // We do NOT overwrite glfwSetWindowUserPointer or any window callbacks.
    // Input events now flow through the causticaEvents system:
    //   GlfwWindow → Application::onEvent() → EditorApplication::onEvent().

    // DPI tracking is handled by GlfwWindow::onMove(). Sync initial values.
    m_DPIScaleFactorX = window->getDPIScaleX();
    m_DPIScaleFactorY = window->getDPIScaleY();

    // Render-during-move: delegate to GlfwWindow
    window->setRenderDuringMove(m_EnableRenderDuringWindowMovement);

    // Create GPU instance, device, and swapchain
    if (!CreateInstance(m_DeviceParams))
    {
        caustica::error("CreateDeviceAndSwapChain: Failed to create GPU instance");
        return false;
    }
    if (!CreateDevice())
        return false;
    if (!CreateSwapChain())
        return false;

    // Reset back buffer state to enforce resize event
    m_DeviceParams.backBufferWidth  = 0;
    m_DeviceParams.backBufferHeight = 0;
    UpdateWindowSize();

    caustica::info("CreateDeviceAndSwapChain: Device ready with platform Window [%ux%u]",
        window->getWidth(), window->getHeight());

    return true;
}

void GpuDevice::BackBufferResizing()
{
    m_SwapChain.framebuffers.clear();
    m_SwapChain.framebuffersWithDepth.clear();

    if (m_frameDriver)
        m_frameDriver->notifyBackBufferResizing();
}

void GpuDevice::BackBufferResized()
{
    CreateDepthBuffer();

    if (m_frameDriver)
        m_frameDriver->notifyBackBufferResized(
            m_DeviceParams.backBufferWidth,
            m_DeviceParams.backBufferHeight,
            m_DeviceParams.swapChainSampleCount);

    uint32_t backBufferCount = GetBackBufferCount();
    m_SwapChain.framebuffers.resize(backBufferCount);
    m_SwapChain.framebuffersWithDepth.resize(backBufferCount);
    for (uint32_t index = 0; index < backBufferCount; index++)
    {
        nvrhi::FramebufferDesc framebufferDesc = nvrhi::FramebufferDesc()
            .addColorAttachment(GetBackBuffer(index));
        
        m_SwapChain.framebuffers[index] = GetDevice()->createFramebuffer(framebufferDesc);

        if (m_SwapChain.depthBuffer)
        {
            framebufferDesc.setDepthAttachment(m_SwapChain.depthBuffer);
            m_SwapChain.framebuffersWithDepth[index] = GetDevice()->createFramebuffer(framebufferDesc);
        }
        else
        {
            m_SwapChain.framebuffersWithDepth[index] = m_SwapChain.framebuffers[index];
        }
    }
}

void GpuDevice::CreateDepthBuffer()
{
    m_SwapChain.depthBuffer = nullptr;

    if (m_DeviceParams.depthBufferFormat == nvrhi::Format::UNKNOWN)
        return;

    nvrhi::TextureDesc textureDesc = nvrhi::TextureDesc()
        .setDebugName("Depth Buffer")
        .setWidth(m_DeviceParams.backBufferWidth)
        .setHeight(m_DeviceParams.backBufferHeight)
        .setFormat(m_DeviceParams.depthBufferFormat)
        .setDimension(m_DeviceParams.swapChainSampleCount > 1
            ? nvrhi::TextureDimension::Texture2DMS
            : nvrhi::TextureDimension::Texture2D)
        .setSampleCount(m_DeviceParams.swapChainSampleCount)
        .setSampleQuality(m_DeviceParams.swapChainSampleQuality)
        .setIsTypeless(true)
        .setIsRenderTarget(true)
        .enableAutomaticStateTracking(nvrhi::ResourceStates::DepthWrite);

    m_SwapChain.depthBuffer = GetDevice()->createTexture(textureDesc);
}

bool GpuDevice::CreateHeadlessBackBuffers()
{
    ReleaseHeadlessBackBuffers();

    if (!GetDevice())
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
        nvrhi::TextureDesc textureDesc = nvrhi::TextureDesc()
            .setDebugName("Headless Back Buffer")
            .setWidth(m_DeviceParams.backBufferWidth)
            .setHeight(m_DeviceParams.backBufferHeight)
            .setFormat(m_DeviceParams.swapChainFormat)
            .setDimension(m_DeviceParams.swapChainSampleCount > 1
                ? nvrhi::TextureDimension::Texture2DMS
                : nvrhi::TextureDimension::Texture2D)
            .setSampleCount(m_DeviceParams.swapChainSampleCount)
            .setSampleQuality(m_DeviceParams.swapChainSampleQuality)
            .setIsRenderTarget(true)
            .setInitialState(nvrhi::ResourceStates::RenderTarget)
            .setKeepInitialState(true);

        nvrhi::TextureHandle texture = GetDevice()->createTexture(textureDesc);
        if (!texture)
        {
            caustica::error("Failed to create headless back buffer %u.", index);
            ReleaseHeadlessBackBuffers();
            return false;
        }

        m_HeadlessBackBuffers.push_back(texture);
    }

    m_HeadlessBackBufferIndex = 0;
    return true;
}

void GpuDevice::ReleaseHeadlessBackBuffers()
{
    m_HeadlessBackBuffers.clear();
    m_HeadlessBackBufferIndex = 0;
}

bool GpuDevice::BeginHeadlessFrame()
{
    return !m_HeadlessBackBuffers.empty();
}

bool GpuDevice::PresentHeadlessFrame()
{
    if (m_HeadlessBackBuffers.empty())
        return false;

    m_HeadlessBackBufferIndex = (m_HeadlessBackBufferIndex + 1) % uint32_t(m_HeadlessBackBuffers.size());
    return true;
}

nvrhi::ITexture* GpuDevice::GetHeadlessBackBuffer(uint32_t index)
{
    if (index < m_HeadlessBackBuffers.size())
        return m_HeadlessBackBuffers[index];

    return nullptr;
}

uint32_t GpuDevice::GetCurrentHeadlessBackBufferIndex() const
{
    return m_HeadlessBackBufferIndex;
}

uint32_t GpuDevice::GetHeadlessBackBufferCount() const
{
    return uint32_t(m_HeadlessBackBuffers.size());
}

void GpuDevice::UpdateAverageFrameTime(double elapsedTime)
{
    m_FrameTimeSum += elapsedTime;
    m_NumberOfAccumulatedFrames += 1;
    
    if (m_FrameTimeSum > m_AverageTimeUpdateInterval && m_NumberOfAccumulatedFrames > 0)
    {
        m_AverageFrameTime = m_FrameTimeSum / double(m_NumberOfAccumulatedFrames);
        m_NumberOfAccumulatedFrames = 0;
        m_FrameTimeSum = 0.0;
    }
}

void GpuDevice::GetDPIScaleInfo(float& x, float& y) const
{
    if (m_WindowPtr) { x = m_WindowPtr->getDPIScaleX(); y = m_WindowPtr->getDPIScaleY(); }
    else            { x = m_DPIScaleFactorX;   y = m_DPIScaleFactorY; }
}

void GpuDevice::GetWindowDimensions(int& width, int& height)
{
    if (m_WindowPtr)
    {
        width  = static_cast<int>(m_WindowPtr->getWidth());
        height = static_cast<int>(m_WindowPtr->getHeight());
    }
    else
    {
        width  = m_DeviceParams.backBufferWidth;
        height = m_DeviceParams.backBufferHeight;
    }
}

const DeviceCreationParameters& GpuDevice::GetDeviceParams()
{
    return m_DeviceParams;
}

caustica::GpuDevice::GpuDevice()
#if CAUSTICA_WITH_AFTERMATH
    : m_AftermathCrashDumper(*this)
#endif
{
}

void GpuDevice::UpdateWindowSize()
{
    if (!m_Window)
        return;

    int width;
    int height;
    glfwGetWindowSize(m_Window, &width, &height);

    if (width == 0 || height == 0)
    {
        m_CanPresentSwapChain = false;
        return;
    }

    m_CanPresentSwapChain = true;

    if (int(m_DeviceParams.backBufferWidth) != width || 
        int(m_DeviceParams.backBufferHeight) != height ||
        (m_DeviceParams.vsyncEnabled != m_RequestedVSync && GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN))
    {
        // window is not minimized, and the size has changed

        BackBufferResizing();

        m_DeviceParams.backBufferWidth = width;
        m_DeviceParams.backBufferHeight = height;
        m_DeviceParams.vsyncEnabled = m_RequestedVSync;

        ResizeSwapChain();
        BackBufferResized();
    }

    m_DeviceParams.vsyncEnabled = m_RequestedVSync;
}

void GpuDevice::Shutdown()
{
#if CAUSTICA_WITH_STREAMLINE
    // Shut down Streamline before destroying swap chain and device.
    StreamlineIntegration::Get().Shutdown();
#endif

    m_SwapChain.framebuffers.clear();
    m_SwapChain.framebuffersWithDepth.clear();
    m_SwapChain.depthBuffer = nullptr;
    ReleaseHeadlessBackBuffers();

    DestroyDeviceAndSwapChain();

    m_Window = nullptr;
    m_WindowPtr = nullptr;

    m_InstanceCreated = false;
}

nvrhi::IFramebuffer* caustica::GpuDevice::GetCurrentFramebuffer(bool withDepth)
{
    return GetFramebuffer(GetCurrentBackBufferIndex(), withDepth);
}

nvrhi::IFramebuffer* caustica::GpuDevice::GetFramebuffer(uint32_t index, bool withDepth)
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

void GpuDevice::SetWindowTitle(const char* title)
{
    assert(title);
    if (m_WindowTitle == title)
        return;

    if (m_Window != nullptr)
        glfwSetWindowTitle(m_Window, title);

    m_WindowTitle = title;
}

void GpuDevice::SetInformativeWindowTitle(const char* applicationName, bool includeFramerate, const char* extraInfo)
{
    std::stringstream ss;
    ss << applicationName;
    ss << " (" << nvrhi::utils::GraphicsAPIToString(GetDevice()->getGraphicsAPI());

    if (m_DeviceParams.enableDebugRuntime)
    {
        if (GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
            ss << ", VulkanValidationLayer";
        else
            ss << ", DebugRuntime";
    }

    if (m_DeviceParams.enableNvrhiValidationLayer)
    {
        ss << ", NvrhiValidationLayer";
    }

    ss << ")";

    double frameTime = GetAverageFrameTimeSeconds();
    if (includeFramerate && frameTime > 0)
    {
        double const fps = 1.0 / frameTime;
        int const precision = (fps <= 20.0) ? 1 : 0;
        ss << " - " << std::fixed << std::setprecision(precision) << fps << " FPS ";
    }

    if (extraInfo)
        ss << extraInfo;

    SetWindowTitle(ss.str().c_str());
}

const char* caustica::GpuDevice::GetWindowTitle()
{
    return m_WindowTitle.c_str();
}

caustica::GpuDevice* caustica::GpuDevice::Create(nvrhi::GraphicsAPI api)
{
    switch (api)
    {
#if CAUSTICA_WITH_DX11
    case nvrhi::GraphicsAPI::D3D11:
        return CreateD3D11();
#endif
#if CAUSTICA_WITH_DX12
    case nvrhi::GraphicsAPI::D3D12:
        return CreateD3D12();
#endif
#if CAUSTICA_WITH_VULKAN
    case nvrhi::GraphicsAPI::VULKAN:
        return CreateVK();
#endif
    default:
        caustica::error("GpuDevice::Create: Unsupported Graphics API (%d)", api);
        return nullptr;
    }
}

DefaultMessageCallback& DefaultMessageCallback::GetInstance()
{
    static DefaultMessageCallback Instance;
    return Instance;
}

void DefaultMessageCallback::message(nvrhi::MessageSeverity severity, const char* messageText)
{
    caustica::Severity logSeverity = caustica::Severity::Info;
    switch (severity)
    {
    case nvrhi::MessageSeverity::Info:
        logSeverity = caustica::Severity::Info;
        break;
    case nvrhi::MessageSeverity::Warning:
        logSeverity = caustica::Severity::Warning;
        break;
    case nvrhi::MessageSeverity::Error:
        logSeverity = caustica::Severity::Error;
        break;
    case nvrhi::MessageSeverity::Fatal:
        logSeverity = caustica::Severity::Fatal;
        break;
    }
    
    caustica::message(logSeverity, "%s", messageText);
}

#if CAUSTICA_WITH_STREAMLINE
StreamlineInterface& GpuDevice::GetStreamline()
{
    // StreamlineIntegration doesn't support instances
    return StreamlineIntegration::Get();
}
#endif

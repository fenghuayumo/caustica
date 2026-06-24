#include <backend/GpuDevice.h>
#include <platform/Input.h>      // Platform layer: input dispatch
#include <platform/window.h>     // Platform layer: Window abstraction
#include <platform/glfw_window.h>// Platform layer: GLFW window
#include <render/RenderPassManager.h>  // Renderer layer: pass management
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


// Window event callbacks for old path (CreateWindowDeviceAndSwapChain).
// New path uses GlfwWindow which handles these via its own callbacks.
static void WindowIconifyCallback_GLFW(GLFWwindow* window, int iconified)
{
    GpuDevice* mgr = reinterpret_cast<GpuDevice*>(glfwGetWindowUserPointer(window));
    mgr->m_windowVisible = (iconified == GLFW_FALSE);
}

static void WindowFocusCallback_GLFW(GLFWwindow* window, int focused)
{
    GpuDevice* mgr = reinterpret_cast<GpuDevice*>(glfwGetWindowUserPointer(window));
    mgr->m_windowIsInFocus = (focused == GLFW_TRUE);
}

static void WindowRefreshCallback_GLFW(GLFWwindow* /*window*/)
{
    // No-op: redraw is driven by the message loop
}

static void WindowCloseCallback_GLFW(GLFWwindow* /*window*/)
{
    // No-op: exit is checked via glfwWindowShouldClose in the message loop
}

static void WindowPosCallback_GLFW(GLFWwindow* /*window*/, int /*xpos*/, int /*ypos*/)
{
    // DPI tracking now handled by GlfwWindow::onMove().
    // Old path: DPI is tracked via UpdateWindowSize in the message loop.
}

static void KeyCallback_GLFW(GLFWwindow *window, int key, int scancode, int action, int mods)
{
    GpuDevice *manager = reinterpret_cast<GpuDevice *>(glfwGetWindowUserPointer(window));
    manager->KeyboardUpdate(key, scancode, action, mods);
}

static void CharModsCallback_GLFW(GLFWwindow *window, unsigned int unicode, int mods)
{
    GpuDevice *manager = reinterpret_cast<GpuDevice *>(glfwGetWindowUserPointer(window));
    manager->KeyboardCharInput(unicode, mods);
}

static void MousePosCallback_GLFW(GLFWwindow *window, double xpos, double ypos)
{
    GpuDevice *manager = reinterpret_cast<GpuDevice *>(glfwGetWindowUserPointer(window));
    manager->MousePosUpdate(xpos, ypos);
}

static void MouseButtonCallback_GLFW(GLFWwindow *window, int button, int action, int mods)
{
    GpuDevice *manager = reinterpret_cast<GpuDevice *>(glfwGetWindowUserPointer(window));
    manager->MouseButtonUpdate(button, action, mods);
}

static void MouseScrollCallback_GLFW(GLFWwindow *window, double xoffset, double yoffset)
{
    GpuDevice *manager = reinterpret_cast<GpuDevice *>(glfwGetWindowUserPointer(window));
    manager->MouseScrollUpdate(xoffset, yoffset);
}

static void JoystickConnectionCallback_GLFW(int joyId, int connectDisconnect)
{
    Input::onJoystickEvent(joyId, connectDisconnect);
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

bool GpuDevice::CreateWindowDeviceAndSwapChain(const DeviceCreationParameters& params, const char *windowTitle)
{
    m_DeviceParams = params;
    m_DeviceParams.headlessDevice = false;
    m_RequestedVSync = params.vsyncEnabled;

#ifndef _WINDOWS
    // This is necessary to get correct window decorations on Wayland
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif

    if (!CreateInstance(m_DeviceParams))
        return false;

    glfwSetErrorCallback(ErrorCallback_GLFW);

    glfwDefaultWindowHints();

    bool foundFormat = false;
    for (const auto& info : formatInfo)
    {
        if (info.format == params.swapChainFormat)
        {
            glfwWindowHint(GLFW_RED_BITS, info.redBits);
            glfwWindowHint(GLFW_GREEN_BITS, info.greenBits);
            glfwWindowHint(GLFW_BLUE_BITS, info.blueBits);
            glfwWindowHint(GLFW_ALPHA_BITS, info.alphaBits);
            glfwWindowHint(GLFW_DEPTH_BITS, info.depthBits);
            glfwWindowHint(GLFW_STENCIL_BITS, info.stencilBits);
            foundFormat = true;
            break;
        }
    }

    if (!foundFormat)
    {
        caustica::error("Unknown format %s (%d) used for the swap chain",
            nvrhi::getFormatInfo(params.swapChainFormat).name,
            int(params.swapChainFormat));
    }

    glfwWindowHint(GLFW_SAMPLES, params.swapChainSampleCount);
    glfwWindowHint(GLFW_REFRESH_RATE, params.refreshRate);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, params.resizeWindowWithDisplayScale);

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);   // Ignored for fullscreen

    if (params.startBorderless)
    {
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE); // Borderless window
    }

    m_Window = glfwCreateWindow(params.backBufferWidth, params.backBufferHeight,
                                windowTitle ? windowTitle : "",
                                params.startFullscreen ? glfwGetPrimaryMonitor() : nullptr,
                                nullptr);

    if (m_Window == nullptr)
    {
        return false;
    }

    if (params.startFullscreen)
    {
        glfwSetWindowMonitor(m_Window, glfwGetPrimaryMonitor(), 0, 0,
            m_DeviceParams.backBufferWidth, m_DeviceParams.backBufferHeight, m_DeviceParams.refreshRate);
    }
    else
    {
        int fbWidth = 0, fbHeight = 0;
        glfwGetFramebufferSize(m_Window, &fbWidth, &fbHeight);
        m_DeviceParams.backBufferWidth = fbWidth;
        m_DeviceParams.backBufferHeight = fbHeight;
    }

    if (windowTitle)
        m_WindowTitle = windowTitle;

    glfwSetWindowUserPointer(m_Window, this);

    if (params.windowPosX != -1 && params.windowPosY != -1)
    {
        glfwSetWindowPos(m_Window, params.windowPosX, params.windowPosY);
    }
    
    glfwSetWindowPosCallback(m_Window, WindowPosCallback_GLFW);
    glfwSetWindowCloseCallback(m_Window, WindowCloseCallback_GLFW);
    glfwSetWindowRefreshCallback(m_Window, WindowRefreshCallback_GLFW);
    glfwSetWindowFocusCallback(m_Window, WindowFocusCallback_GLFW);
    glfwSetWindowIconifyCallback(m_Window, WindowIconifyCallback_GLFW);
    glfwSetKeyCallback(m_Window, KeyCallback_GLFW);
    glfwSetCharModsCallback(m_Window, CharModsCallback_GLFW);
    glfwSetCursorPosCallback(m_Window, MousePosCallback_GLFW);
    glfwSetMouseButtonCallback(m_Window, MouseButtonCallback_GLFW);
    glfwSetScrollCallback(m_Window, MouseScrollCallback_GLFW);
    glfwSetJoystickCallback(JoystickConnectionCallback_GLFW);

    // If there are multiple device managers, then this would be called by each one which isn't necessary
    // but should not hurt.

    if (!CreateDevice())
        return false;

    if (!CreateSwapChain())
        return false;

    glfwShowWindow(m_Window);
    
    if (m_DeviceParams.startMaximized)
    {
        glfwMaximizeWindow(m_Window);
    }

    // reset the back buffer size state to enforce a resize event
    m_DeviceParams.backBufferWidth = 0;
    m_DeviceParams.backBufferHeight = 0;

    UpdateWindowSize();

    return true;
}

// ---------------------------------------------------------------------------
// 4-layer architecture bridge:
// Create GPU device + swapchain using a pre-existing Window from the platform layer.
// This allows the new Application to own the Window independently of GpuDevice.
// ---------------------------------------------------------------------------
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

    // Install Input (keyboard/mouse) callbacks on the GLFW window.
    if (!m_Input) m_Input = new Input();
    m_Input->installCallbacks(glfwHandle);  // Pass GLFWwindow*, not native handle

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

void GpuDevice::AddRenderPassToFront(IRenderPass *pRenderPass)
{
    m_vRenderPasses.remove(pRenderPass);
    m_vRenderPasses.push_front(pRenderPass);

    // Delegate to Renderer layer
    if (!m_PassManager) m_PassManager = new RenderPassManager();
    m_PassManager->addToFront(pRenderPass,
        m_DeviceParams.backBufferWidth,
        m_DeviceParams.backBufferHeight,
        m_DeviceParams.swapChainSampleCount);

    // Also register with the Input layer (if it implements IInputHandler)
    if (!m_Input) m_Input = new Input();
    if (auto* handler = dynamic_cast<IInputHandler*>(pRenderPass))
        m_Input->registerHandler(handler);
}

void GpuDevice::AddRenderPassToBack(IRenderPass *pRenderPass)
{
    m_vRenderPasses.remove(pRenderPass);
    m_vRenderPasses.push_back(pRenderPass);

    // Delegate to Renderer layer
    if (!m_PassManager) m_PassManager = new RenderPassManager();
    m_PassManager->addToBack(pRenderPass,
        m_DeviceParams.backBufferWidth,
        m_DeviceParams.backBufferHeight,
        m_DeviceParams.swapChainSampleCount);

    // Also register with the Input layer (if it implements IInputHandler)
    if (!m_Input) m_Input = new Input();
    if (auto* handler = dynamic_cast<IInputHandler*>(pRenderPass))
        m_Input->registerHandler(handler);
}

void GpuDevice::RemoveRenderPass(IRenderPass *pRenderPass)
{
    m_vRenderPasses.remove(pRenderPass);
    if (m_PassManager) m_PassManager->remove(pRenderPass);
    if (m_Input) { if (auto* h = dynamic_cast<IInputHandler*>(pRenderPass)) m_Input->unregisterHandler(h); }
}

void GpuDevice::BackBufferResizing()
{
    m_SwapChain.framebuffers.clear();
    m_SwapChain.framebuffersWithDepth.clear();

    if (m_PassManager)
        m_PassManager->notifyResizing();
    else
        for (auto it : m_vRenderPasses)
            it->BackBufferResizing();
}

void GpuDevice::BackBufferResized()
{
    CreateDepthBuffer();

    if (m_PassManager)
        m_PassManager->notifyResized(m_DeviceParams.backBufferWidth, m_DeviceParams.backBufferHeight, m_DeviceParams.swapChainSampleCount);
    else
        for(auto it : m_vRenderPasses)
            it->BackBufferResized(m_DeviceParams.backBufferWidth, m_DeviceParams.backBufferHeight, m_DeviceParams.swapChainSampleCount);

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

void GpuDevice::DisplayScaleChanged()
{
    for(auto it : m_vRenderPasses)
    {
        it->DisplayScaleChanged(m_DPIScaleFactorX, m_DPIScaleFactorY);
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

void GpuDevice::Animate(double elapsedTime, bool windowIsFocused)
{
    for(auto it : m_vRenderPasses)
    {
        if (windowIsFocused || it->ShouldAnimateUnfocused())
        {
            it->Animate(float(elapsedTime));
            it->SetLatewarpOptions();
        }
    }
}

void GpuDevice::Render()
{
    for (auto it : m_vRenderPasses)
    {
        it->Render(GetCurrentFramebuffer(it->SupportsDepthBuffer()));
    }
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

bool GpuDevice::ShouldRenderUnfocused() const
{
    for (auto it = m_vRenderPasses.crbegin(); it != m_vRenderPasses.crend(); it++)
    {
        bool ret = (*it)->ShouldRenderUnfocused();
        if (ret)
            return true;
    }

    return false;
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
    int width;
    int height;
    glfwGetWindowSize(m_Window, &width, &height);

    if (width == 0 || height == 0)
    {
        // window is minimized
        m_windowVisible = false;
        return;
    }

    m_windowVisible = true;

    m_windowIsInFocus = glfwGetWindowAttrib(m_Window, GLFW_FOCUSED) == 1;

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

void GpuDevice::KeyboardUpdate(int key, int scancode, int action, int mods)
    { if (m_Input) m_Input->onKey(key, scancode, action, mods); }
void GpuDevice::KeyboardCharInput(unsigned int unicode, int mods)
    { if (m_Input) m_Input->onChar(unicode, mods); }
void GpuDevice::MousePosUpdate(double xpos, double ypos)
{
    if (!m_DeviceParams.supportExplicitDisplayScaling) { xpos /= m_DPIScaleFactorX; ypos /= m_DPIScaleFactorY; }
    if (m_Input) m_Input->onMouseMove(xpos, ypos);
}
void GpuDevice::MouseButtonUpdate(int button, int action, int mods)
    { if (m_Input) m_Input->onMouseButton(button, action, mods); }
void GpuDevice::MouseScrollUpdate(double xoffset, double yoffset)
    { if (m_Input) m_Input->onMouseScroll(xoffset, yoffset); }








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

    // New path: GlfwWindow owns the GLFW window, don't double-destroy
    if (m_Window && !m_WindowPtr)
    {
        glfwDestroyWindow(m_Window);
        m_Window = nullptr;
        glfwTerminate();
    }

    m_WindowPtr = nullptr;
    delete m_Input;
    m_Input = nullptr;
    delete m_PassManager;
    m_PassManager = nullptr;

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

#include "platform/glfw_window.h"
#include "platform/window.h"

#include <events/event.h>
#include <events/key_event.h>
#include <events/mouse_event.h>
#include <events/application_event.h>

#include <core/log.h>

#include <GLFW/glfw3.h>
#include <algorithm>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <shellscalingapi.h>
#pragma comment(lib, "Shcore.lib")
#endif

namespace caustica
{

// GLFW → caustica key/modifier conversion helpers (zero-cost: values match 1:1)
namespace
{
inline constexpr KeyCode    FromGlfwKey(int glfwKey)    { return static_cast<KeyCode>(glfwKey); }
inline constexpr MouseCode   FromGlfwMouse(int glfwBtn) { return static_cast<MouseCode>(glfwBtn); }
inline constexpr KeyAction   FromGlfwAction(int action) { return static_cast<KeyAction>(action); }
inline constexpr ModifierKey FromGlfwMods(int mods)     { return static_cast<ModifierKey>(mods); }
} // anonymous namespace

} // namespace caustica

namespace caustica
{

// ---------------------------------------------------------------------------
// Static factory registration
// ---------------------------------------------------------------------------
void GlfwWindow::makeDefault()
{
    Window::s_CreateFunc = GlfwWindow::createGlfwWindow;
}

Window* GlfwWindow::createGlfwWindow(const WindowDesc& desc)
{
    auto* w = new GlfwWindow();
    if (!w->initialise(desc))
    {
        delete w;
        return nullptr;
    }
    return w;
}

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------
GlfwWindow::~GlfwWindow()
{
    if (m_Window)
    {
        glfwDestroyWindow(m_Window);
        m_Window = nullptr;
    }
}

bool GlfwWindow::initialise(const WindowDesc& desc)
{
    m_Title = desc.Title;
    m_VSync = desc.VSync;

    // Streamline DLSS-RR needs a real HWND. Windowed EngineApp / Python
    // create the GLFW window before GpuDevice::createInstance, so GLFW
    // must be initialized here rather than later in the device path.
    glfwSetErrorCallback(glfwErrorCallback);
#if !defined(_WIN32) && defined(GLFW_PLATFORM) && defined(GLFW_PLATFORM_X11)
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
    if (!glfwInit())
    {
        caustica::error("[GlfwWindow] glfwInit failed");
        return false;
    }

    // Window hints
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    // Be explicit: stale platform/window-manager state must not turn a normal
    // editor window into a borderless one.
    glfwWindowHint(GLFW_DECORATED, desc.Borderless ? GLFW_FALSE : GLFW_TRUE);

    glfwWindowHint(GLFW_SAMPLES, 1);

    GLFWmonitor* monitor = desc.Fullscreen ? glfwGetPrimaryMonitor() : nullptr;

    m_Window = glfwCreateWindow(
        static_cast<int>(desc.Width),
        static_cast<int>(desc.Height),
        desc.Title.c_str(),
        monitor,
        nullptr);

    if (!m_Window)
    {
        caustica::error("[GlfwWindow] Failed to create GLFW window");
        return false;
    }
    // store this pointer for GLFW callbacks
    glfwSetWindowUserPointer(m_Window, this);

    // Register GLFW callbacks
    glfwSetKeyCallback(m_Window, glfwKeyCallback);
    glfwSetCharCallback(m_Window, glfwCharCallback);
    glfwSetCursorPosCallback(m_Window, glfwCursorPosCallback);
    glfwSetMouseButtonCallback(m_Window, glfwMouseButtonCallback);
    glfwSetScrollCallback(m_Window, glfwScrollCallback);
    glfwSetWindowCloseCallback(m_Window, glfwWindowCloseCallback);
    glfwSetWindowSizeCallback(m_Window, glfwWindowSizeCallback);
    glfwSetWindowFocusCallback(m_Window, glfwWindowFocusCallback);
    glfwSetWindowIconifyCallback(m_Window, glfwWindowIconifyCallback);
    glfwSetWindowPosCallback(m_Window, glfwWindowPosCallback);
    glfwSetWindowRefreshCallback(m_Window, [](GLFWwindow* w) {
        auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(w));
        if (self) self->onRefresh();
    });
    glfwSetDropCallback(m_Window, glfwDropCallback);

    // Set icon if provided
    if (!desc.IconPaths.empty() || !desc.IconData.empty())
    {
        setIcon(desc);
    }

    // Windowed (non-maximized) mode: center on the primary monitor work area so
    // the title bar and close button stay visible. Skip when maximized —
    // glfwSetWindowPos restores from maximize and can push the caption off-screen.
    if (!desc.Fullscreen && !desc.Maximized)
    {
        if (GLFWmonitor* monitor = glfwGetPrimaryMonitor())
        {
            int workX = 0, workY = 0, workW = 0, workH = 0;
            glfwGetMonitorWorkarea(monitor, &workX, &workY, &workW, &workH);
            int winW = 0, winH = 0;
            glfwGetWindowSize(m_Window, &winW, &winH);
            if (workW > 0 && workH > 0 && winW > 0 && winH > 0)
            {
                const int posX = workX + std::max(0, (workW - winW) / 2);
                const int posY = workY + std::max(0, (workH - winH) / 2);
                glfwSetWindowPos(m_Window, posX, posY);
            }
        }
    }

    glfwShowWindow(m_Window);

    m_Init = true;
    return true;
}

// ---------------------------------------------------------------------------
// Window interface implementation
// ---------------------------------------------------------------------------
bool GlfwWindow::getExit() const
{
    return m_ExitRequested || glfwWindowShouldClose(m_Window);
}

void GlfwWindow::setExit(bool exit)
{
    m_ExitRequested = exit;
    glfwSetWindowShouldClose(m_Window, exit ? GLFW_TRUE : GLFW_FALSE);
}

uint32_t GlfwWindow::getWidth() const
{
    int w = 0, h = 0;
    glfwGetWindowSize(m_Window, &w, &h);
    return static_cast<uint32_t>(w);
}

uint32_t GlfwWindow::getHeight() const
{
    int w = 0, h = 0;
    glfwGetWindowSize(m_Window, &w, &h);
    return static_cast<uint32_t>(h);
}

float GlfwWindow::getDPIScale() const
{
    return m_DPIScaleX;
}

float GlfwWindow::getScreenRatio() const
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_Window, &w, &h);
    if (h == 0) return 1.0f;
    return static_cast<float>(w) / static_cast<float>(h);
}

std::string GlfwWindow::getTitle() const
{
    return m_Title;
}

void* GlfwWindow::getNativeHandle()
{
#ifdef _WIN32
    return glfwGetWin32Window(m_Window);
#else
    return m_Window;
#endif
}

std::array<uint32_t, 2> GlfwWindow::getFramebufferSize() const
{
    int w = 0, h = 0;
    glfwGetFramebufferSize(m_Window, &w, &h);
    return {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
}

void GlfwWindow::setWindowTitle(const std::string& title)
{
    m_Title = title;
    glfwSetWindowTitle(m_Window, title.c_str());
}

void GlfwWindow::toggleVSync()
{
    m_VSync = !m_VSync;
}

void GlfwWindow::setVSync(bool set)
{
    m_VSync = set;
}

void GlfwWindow::setBorderless(bool borderless)
{
    glfwSetWindowAttrib(m_Window, GLFW_DECORATED, borderless ? GLFW_FALSE : GLFW_TRUE);
}

void GlfwWindow::maximise()
{
    glfwMaximizeWindow(m_Window);
}

void GlfwWindow::hide()
{
    if (m_Window)
        glfwHideWindow(m_Window);
}

void GlfwWindow::setSize(uint32_t width, uint32_t height)
{
    if (m_Window)
        glfwSetWindowSize(m_Window, static_cast<int>(width), static_cast<int>(height));
}

void GlfwWindow::hideMouse(bool hide)
{
    if (hide)
        glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    else
        glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}

void GlfwWindow::setCursorVisible(bool visible)
{
    if (m_Window)
        glfwSetInputMode(m_Window, GLFW_CURSOR, visible ? GLFW_CURSOR_NORMAL : GLFW_CURSOR_HIDDEN);
}

void GlfwWindow::setMousePosition(float x, float y)
{
    glfwSetCursorPos(m_Window, static_cast<double>(x), static_cast<double>(y));
}

void GlfwWindow::setIcon(const WindowDesc& desc)
{
    // GLFW image conversion from paths or data
    // Simplified: not implementing full icon loading here;
    // the existing GpuDevice icon support can be wired
    (void)desc;
}

void GlfwWindow::onUpdate()
{
    glfwPollEvents();
}

void GlfwWindow::setEventCallback(const EventCallbackFn& callback)
{
    m_EventCallback = callback;
}

// ---------------------------------------------------------------------------
// Static GLFW callbacks
// ---------------------------------------------------------------------------
void GlfwWindow::glfwErrorCallback(int error, const char* description)
{
    caustica::error("GLFW error [%d]: %s", error, description ? description : "");
}

void GlfwWindow::glfwKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (!self || !self->m_EventCallback)
        return;

    auto keyAction  = FromGlfwAction(action);
    auto keyCode    = FromGlfwKey(key);
    auto modifiers  = FromGlfwMods(mods);

    if (keyAction == KeyAction::Release)
    {
        self->m_EventCallback(std::make_unique<KeyReleasedEvent>(keyCode, scancode, modifiers));
    }
    else
    {
        int repeatCount = (keyAction == KeyAction::Repeat) ? 1 : 0;
        self->m_EventCallback(std::make_unique<KeyPressedEvent>(keyCode, scancode, repeatCount, modifiers));
    }
}

void GlfwWindow::glfwCharCallback(GLFWwindow* window, unsigned int codepoint)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self && self->m_EventCallback)
    {
        self->m_EventCallback(std::make_unique<KeyTypedEvent>(codepoint));
    }
}

void GlfwWindow::glfwCursorPosCallback(GLFWwindow* window, double xpos, double ypos)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self && self->m_EventCallback)
    {
        self->m_EventCallback(std::make_unique<MouseMovedEvent>(xpos, ypos));
    }
}

void GlfwWindow::glfwMouseButtonCallback(GLFWwindow* window, int button, int action, int mods)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (!self || !self->m_EventCallback)
        return;

    auto mouseCode = FromGlfwMouse(button);
    auto modifiers = FromGlfwMods(mods);

    if (action == GLFW_PRESS)
        self->m_EventCallback(std::make_unique<MouseButtonPressedEvent>(mouseCode, modifiers));
    else
        self->m_EventCallback(std::make_unique<MouseButtonReleasedEvent>(mouseCode, modifiers));
}

void GlfwWindow::glfwScrollCallback(GLFWwindow* window, double xoffset, double yoffset)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self && self->m_EventCallback)
    {
        self->m_EventCallback(std::make_unique<MouseScrolledEvent>(xoffset, yoffset));
    }
}

void GlfwWindow::glfwWindowCloseCallback(GLFWwindow* window)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self && self->m_EventCallback)
    {
        self->m_EventCallback(std::make_unique<WindowCloseEvent>());
    }
}

void GlfwWindow::glfwWindowSizeCallback(GLFWwindow* window, int width, int height)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self && self->m_EventCallback)
    {
        self->m_HasResized = true;
        self->m_EventCallback(std::make_unique<WindowResizeEvent>(width, height));
    }
}

void GlfwWindow::glfwWindowFocusCallback(GLFWwindow* window, int focused)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (!self)
        return;

    self->onFocusChanged(focused == GLFW_TRUE);

    if (self->m_EventCallback)
    {
        if (focused == GLFW_TRUE)
            self->m_EventCallback(std::make_unique<WindowFocusEvent>());
        else
            self->m_EventCallback(std::make_unique<WindowLostFocusEvent>());
    }
}

void GlfwWindow::glfwWindowIconifyCallback(GLFWwindow* window, int iconified)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (!self)
        return;

    self->onIconifyChanged(iconified == GLFW_TRUE);

    if (self->m_EventCallback)
    {
        self->m_EventCallback(std::make_unique<WindowIconifyEvent>(iconified == GLFW_TRUE));
    }
}

void GlfwWindow::glfwDropCallback(GLFWwindow* window, int count, const char** paths)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self && self->m_FileDropCallback)
        self->m_FileDropCallback(count, paths);
}

void GlfwWindow::glfwWindowPosCallback(GLFWwindow* window, int xpos, int ypos)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (!self)
        return;

    self->onMove(xpos, ypos);

    if (self->m_EventCallback)
    {
        self->m_EventCallback(std::make_unique<WindowMovedEvent>(xpos, ypos));
    }
}

// ---------------------------------------------------------------------------
// Window event overrides
// ---------------------------------------------------------------------------

void GlfwWindow::onFocusChanged(bool focused)
{
    Window::onFocusChanged(focused);
    // Forward to Application via event callback
}

void GlfwWindow::onIconifyChanged(bool iconified)
{
    Window::onIconifyChanged(iconified);
    // Forward to Application via event callback
}

void GlfwWindow::onMove(int x, int y)
{
    Window::onMove(x, y);
    m_PosX = static_cast<float>(x);
    m_PosY = static_cast<float>(y);

    // DPI tracking (moved from GpuDevice::WindowPosCallback)
#ifdef _WIN32
    HWND hwnd = glfwGetWin32Window(m_Window);
    if (hwnd)
    {
        auto monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        unsigned int dpiX = 0, dpiY = 0;
        if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
        {
            m_DPIScaleX = dpiX / 96.0f;
            m_DPIScaleY = dpiY / 96.0f;
        }
    }
#else
    GLFWmonitor* monitor = glfwGetWindowMonitor(m_Window);
    if (!monitor)
        monitor = glfwGetPrimaryMonitor();
    if (monitor)
        glfwGetMonitorContentScale(monitor, &m_DPIScaleX, &m_DPIScaleY);
#endif

    // render-during-move (was in GpuDevice)
    if (m_RenderDuringMove && m_OnRenderDuringMove)
        m_OnRenderDuringMove();
}

} // namespace caustica


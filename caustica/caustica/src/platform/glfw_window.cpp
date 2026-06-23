#include "platform/glfw_window.h"
#include "platform/window.h"

#include <GLFW/glfw3.h>
#include <cstdio>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <shellscalingapi.h>
#pragma comment(lib, "Shcore.lib")
#endif

// Forward declare event types (will be in engine/events/ once Phase B is done)
// For now we define minimal event structs inline so the window layer compiles standalone.
namespace caustica
{

// Minimal event forward declarations (will be moved to engine/events/ in Phase B)
struct Event
{
    virtual ~Event() = default;
    virtual bool handled() const { return m_Handled; }
    void setHandled(bool h = true) { m_Handled = h; }
protected:
    bool m_Handled = false;
};

struct WindowCloseEvent : Event {};
struct WindowResizeEvent : Event
{
    WindowResizeEvent(int w, int h) : m_Width(w), m_Height(h) {}
    int getWidth() const  { return m_Width; }
    int getHeight() const { return m_Height; }
private:
    int m_Width, m_Height;
};

struct KeyEvent : Event
{
    KeyEvent(int k, int s, int a, int m) : key(k), scancode(s), action(a), mods(m) {}
    int key, scancode, action, mods;
};

struct MouseMoveEvent : Event
{
    MouseMoveEvent(double x, double y) : xpos(x), ypos(y) {}
    double xpos, ypos;
};

struct MouseButtonEvent : Event
{
    MouseButtonEvent(int b, int a, int m) : button(b), action(a), mods(m) {}
    int button, action, mods;
};

struct MouseScrollEvent : Event
{
    MouseScrollEvent(double x, double y) : xoffset(x), yoffset(y) {}
    double xoffset, yoffset;
};

struct CharInputEvent : Event
{
    CharInputEvent(unsigned int c) : codepoint(c) {}
    unsigned int codepoint;
};

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

    // Window hints
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    if (desc.Borderless)
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);

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
        fprintf(stderr, "[GlfwWindow] Failed to create GLFW window\n");
        return false;
    }
    // Store this pointer for GLFW callbacks
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

    // Position the window if requested
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

void GlfwWindow::hideMouse(bool hide)
{
    if (hide)
        glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    else
        glfwSetInputMode(m_Window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
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

void GlfwWindow::processInput()
{
    // Input polling is done via GLFW callbacks → events
}

void GlfwWindow::updateCursorImgui()
{
    // ImGui cursor updates — handled by ImGuiManager via the callback
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
    fprintf(stderr, "GLFW error [%d]: %s\n", error, description);
}

void GlfwWindow::glfwKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self && self->m_EventCallback)
    {
        KeyEvent e(key, scancode, action, mods);
        self->m_EventCallback(e);
    }
}

void GlfwWindow::glfwCharCallback(GLFWwindow* window, unsigned int codepoint)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self && self->m_EventCallback)
    {
        CharInputEvent e(codepoint);
        self->m_EventCallback(e);
    }
}

void GlfwWindow::glfwCursorPosCallback(GLFWwindow* window, double xpos, double ypos)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self && self->m_EventCallback)
    {
        MouseMoveEvent e(xpos, ypos);
        self->m_EventCallback(e);
    }
}

void GlfwWindow::glfwMouseButtonCallback(GLFWwindow* window, int button, int action, int mods)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self && self->m_EventCallback)
    {
        MouseButtonEvent e(button, action, mods);
        self->m_EventCallback(e);
    }
}

void GlfwWindow::glfwScrollCallback(GLFWwindow* window, double xoffset, double yoffset)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self && self->m_EventCallback)
    {
        MouseScrollEvent e(xoffset, yoffset);
        self->m_EventCallback(e);
    }
}

void GlfwWindow::glfwWindowCloseCallback(GLFWwindow* window)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self && self->m_EventCallback)
    {
        WindowCloseEvent e;
        self->m_EventCallback(e);
    }
}

void GlfwWindow::glfwWindowSizeCallback(GLFWwindow* window, int width, int height)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self && self->m_EventCallback)
    {
        self->m_HasResized = true;
        WindowResizeEvent e(width, height);
        self->m_EventCallback(e);
    }
}

void GlfwWindow::glfwWindowFocusCallback(GLFWwindow* window, int focused)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self)
        self->onFocusChanged(focused == GLFW_TRUE);
}

void GlfwWindow::glfwWindowIconifyCallback(GLFWwindow* window, int iconified)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self)
        self->onIconifyChanged(iconified == GLFW_TRUE);
}

void GlfwWindow::glfwDropCallback(GLFWwindow* window, int count, const char** paths)
{
    (void)window;
    (void)count;
    (void)paths;
}

void GlfwWindow::glfwWindowPosCallback(GLFWwindow* window, int xpos, int ypos)
{
    auto* self = static_cast<GlfwWindow*>(glfwGetWindowUserPointer(window));
    if (self)
        self->onMove(xpos, ypos);
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

    // Render-during-move (was in GpuDevice)
    if (m_RenderDuringMove && m_OnRenderDuringMove)
        m_OnRenderDuringMove();
}

} // namespace caustica


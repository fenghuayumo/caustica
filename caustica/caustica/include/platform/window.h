#pragma once

#include <string>
#include <vector>
#include <array>
#include <functional>
#include <cstdint>

namespace caustica
{

// Forward declarations for the event system
class Event;

// Window creation descriptor
struct WindowDesc
{
    uint32_t Width  = 1280;
    uint32_t Height = 720;
    bool Fullscreen   = false;
    bool Maximized    = false;
    bool VSync        = true;
    bool Borderless   = false;
    bool Resizeable   = true;
    bool ShowConsole  = true;
    std::string Title = "caustica";
    int  RenderAPI    = 1;       // Maps to caustica::rhi::GraphicsAPI
    std::string FilePath;

    std::vector<std::string> IconPaths;
    std::vector<std::pair<uint32_t, uint8_t*>> IconData;

    WindowDesc() = default;

    WindowDesc(uint32_t width, uint32_t height, int renderAPI = 0,
               const std::string& title = "caustica", bool fullscreen = false,
               bool vSync = true, bool borderless = false,
               const std::string& filepath = "")
        : Width(width), Height(height), Title(title)
        , Fullscreen(fullscreen), VSync(vSync)
        , Borderless(borderless), RenderAPI(renderAPI)
        , FilePath(filepath)
    {}
};

// =============================================================================
// Window — Platform layer: OS window abstraction.
//
// Owns the native window handle (GLFWwindow*, HWND, etc.) and tracks all
// window state: visibility, focus, size, position, DPI scale.
//
// Events (close, resize, focus, iconify, move) are dispatched through the
// EventCallback or overridable virtual methods.
// =============================================================================
class Window
{
public:
    using EventCallbackFn = std::function<void(Event&)>;
    using FileDropCallbackFn = std::function<void(int count, const char** paths)>;

    // Factory
    static Window* create(const WindowDesc& desc);

    virtual ~Window() = default;

    // --- Lifecycle ---
    virtual bool hasInitialised() const { return m_Init; }

    // --- Window state queries ---
    virtual bool     getExit() const                    = 0;
    virtual void     setExit(bool exit)                 = 0;
    virtual uint32_t getWidth() const                   = 0;
    virtual uint32_t getHeight() const                  = 0;
    virtual float    getDPIScale() const                { return 1.0f; }
    virtual float    getDPIScaleX() const               { return m_DPIScaleX; }
    virtual float    getDPIScaleY() const               { return m_DPIScaleY; }
    virtual float    getScreenRatio() const             = 0;
    virtual bool     getVSync() const                   { return m_VSync; }
    virtual bool     isVisible() const                  { return m_Visible; }
    virtual bool     isFocused() const                  { return m_HasFocus; }
    virtual std::string getTitle() const                = 0;
    virtual void*    getNativeHandle()                  { return nullptr; }
    virtual std::array<uint32_t, 2> getFramebufferSize() const = 0;

    // --- Window controls ---
    virtual void setWindowTitle(const std::string& title) = 0;
    virtual void toggleVSync()                             = 0;
    virtual void setVSync(bool set)                        = 0;
    virtual void setBorderless(bool borderless)            = 0;
    virtual void maximise()                                {}
    virtual void hideMouse(bool hide)                      {}
    virtual void setMousePosition(float x, float y)        {}
    virtual void setIcon(const WindowDesc& desc)           = 0;

    // --- Per-frame updates ---
    virtual void onUpdate()           = 0;
    virtual void processInput()       {}
    virtual void updateCursorImgui()  = 0;

    // --- Event callback (set by Application) ---
    virtual void setEventCallback(const EventCallbackFn& callback) = 0;

    // --- File drag-and-drop (GLFW-backed windows only) ---
    void setFileDropCallback(FileDropCallbackFn callback) { m_FileDropCallback = std::move(callback); }

    // --- Window events (called from GLFW callbacks, override to hook) ---
    virtual void onClose()             {}
    virtual void onFocusChanged(bool focused);
    virtual void onIconifyChanged(bool iconified);
    virtual void onMove(int x, int y);
    virtual void onRefresh()           {}

    // --- render-while-moving (was in GpuDevice) ---
    void setRenderDuringMove(bool enable) { m_RenderDuringMove = enable; }
    bool getRenderDuringMove() const      { return m_RenderDuringMove; }

    // --- Resize tracking ---
    void setHasResized(bool resized) { m_HasResized = resized; }
    bool getHasResized() const       { return m_HasResized; }

protected:
    static Window* (*s_CreateFunc)(const WindowDesc&);

    Window() = default;

    bool m_Init        = false;
    bool m_VSync       = false;
    bool m_Visible     = true;
    bool m_HasFocus    = true;
    bool m_HasResized  = false;
    bool m_RenderDuringMove = false;
    float m_DPIScaleX  = 1.0f;
    float m_DPIScaleY  = 1.0f;
    float m_PosX       = 0.0f;
    float m_PosY       = 0.0f;
    FileDropCallbackFn m_FileDropCallback;
};

} // namespace caustica

#pragma once

#include <string>
#include <vector>
#include <array>
#include <functional>
#include <cstdint>

namespace caustica
{

// Forward declarations for the event system (defined later in engine/events/)
class Event;

// Window creation descriptor
struct WindowDesc
{
    uint32_t Width  = 1280;
    uint32_t Height = 720;
    bool Fullscreen   = false;
    bool VSync        = true;
    bool Borderless   = false;
    bool Resizeable   = true;
    bool ShowConsole  = true;
    std::string Title = "caustica";
    int  RenderAPI    = 1;       // Maps to nvrhi::GraphicsAPI
    std::string FilePath;

    // Window icon paths (platform-specific)
    std::vector<std::string> IconPaths;

    // Raw icon data: <width, pixel_data>
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

// Platform window abstraction.
// Concrete implementations wrap the platform windowing system (GLFW, Win32, etc.)
class Window
{
public:
    using EventCallbackFn = std::function<void(Event&)>;

    // Factory: creates a platform-appropriate window. The default factory
    // function is set by the OS layer during init().
    static Window* create(const WindowDesc& desc);

    virtual ~Window() = default;

    // Returns true if the underlying window was successfully created
    virtual bool hasInitialised() const { return m_Init; }

    // Window state queries
    virtual bool   getExit() const                = 0;
    virtual void   setExit(bool exit)             = 0;
    virtual uint32_t getWidth() const             = 0;
    virtual uint32_t getHeight() const            = 0;
    virtual float  getDPIScale() const            { return 1.0f; }
    virtual float  getScreenRatio() const         = 0;
    virtual bool   getVSync() const               { return m_VSync; }
    virtual std::string getTitle() const          = 0;
    virtual void*  getNativeHandle()              { return nullptr; }

    // Returns the framebuffer size in pixels (may differ from window size on HiDPI)
    virtual std::array<uint32_t, 2> getFramebufferSize() const = 0;

    // Window controls
    virtual void setWindowTitle(const std::string& title) = 0;
    virtual void toggleVSync()                             = 0;
    virtual void setVSync(bool set)                        = 0;
    virtual void setBorderless(bool borderless)            = 0;
    virtual void maximise()                                {}
    virtual void hideMouse(bool hide)                      {}
    virtual void setMousePosition(float x, float y)             {}
    virtual void setIcon(const WindowDesc& desc)           = 0;

    // Per-frame updates
    virtual void onUpdate()           = 0;
    virtual void processInput()       {}
    virtual void updateCursorImgui()  = 0;

    // Event callback (set by Application)
    virtual void setEventCallback(const EventCallbackFn& callback) = 0;

    // Window state
    void setHasResized(bool resized) { m_HasResized = resized; }
    bool getHasResized() const       { return m_HasResized; }
    void setWindowFocus(bool focus)  { m_WindowFocus = focus; }
    bool getWindowFocus() const      { return m_WindowFocus; }

protected:
    // The static factory function pointer, set by platform init
    static Window* (*s_CreateFunc)(const WindowDesc&);

    Window() = default;

    bool m_Init        = false;
    bool m_VSync       = false;
    bool m_HasResized  = false;
    bool m_WindowFocus = true;
    float m_PosX       = 0.0f;
    float m_PosY       = 0.0f;
};

} // namespace caustica

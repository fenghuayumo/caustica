#pragma once

#include "events/event.h"

#include <sstream>

namespace caustica
{

// =============================================================================
// WindowResizeEvent — framebuffer size changed.
// =============================================================================
class WindowResizeEvent : public Event
{
public:
    WindowResizeEvent(int width, int height)
        : m_Width(width), m_Height(height)
    {}

    int GetWidth()  const { return m_Width; }
    int GetHeight() const { return m_Height; }

    EVENT_CLASS_TYPE(WindowResize);
    EVENT_CLASS_CATEGORY(Application);

    const char* GetName() const override { return "WindowResize"; }
    std::string ToString() const override
    {
        std::ostringstream ss;
        ss << "WindowResize: " << m_Width << "x" << m_Height;
        return ss.str();
    }

private:
    int m_Width;
    int m_Height;
};

// =============================================================================
// WindowCloseEvent — window close button clicked or programmatic exit.
// =============================================================================
class WindowCloseEvent : public Event
{
public:
    EVENT_CLASS_TYPE(WindowClose);
    EVENT_CLASS_CATEGORY(Application);

    const char* GetName() const override { return "WindowClose"; }
};

// =============================================================================
// WindowFocusEvent — window gained input focus.
// =============================================================================
class WindowFocusEvent : public Event
{
public:
    EVENT_CLASS_TYPE(WindowFocus);
    EVENT_CLASS_CATEGORY(Application);

    const char* GetName() const override { return "WindowFocus"; }
};

// =============================================================================
// WindowLostFocusEvent — window lost input focus.
// =============================================================================
class WindowLostFocusEvent : public Event
{
public:
    EVENT_CLASS_TYPE(WindowLostFocus);
    EVENT_CLASS_CATEGORY(Application);

    const char* GetName() const override { return "WindowLostFocus"; }
};

// =============================================================================
// WindowMovedEvent — window position changed.
// =============================================================================
class WindowMovedEvent : public Event
{
public:
    WindowMovedEvent(int x, int y) : m_X(x), m_Y(y) {}

    int GetX() const { return m_X; }
    int GetY() const { return m_Y; }

    EVENT_CLASS_TYPE(WindowMoved);
    EVENT_CLASS_CATEGORY(Application);

    const char* GetName() const override { return "WindowMoved"; }
    std::string ToString() const override
    {
        std::ostringstream ss;
        ss << "WindowMoved: " << m_X << "," << m_Y;
        return ss.str();
    }

private:
    int m_X, m_Y;
};

// =============================================================================
// WindowIconifyEvent — window minimized/restored.
// =============================================================================
class WindowIconifyEvent : public Event
{
public:
    explicit WindowIconifyEvent(bool iconified) : m_Iconified(iconified) {}

    bool IsIconified() const { return m_Iconified; }

    EVENT_CLASS_TYPE(WindowIconify);
    EVENT_CLASS_CATEGORY(Application);

    const char* GetName() const override { return m_Iconified ? "WindowIconified" : "WindowRestored"; }
    std::string ToString() const override
    {
        std::ostringstream ss;
        ss << (m_Iconified ? "WindowIconified" : "WindowRestored");
        return ss.str();
    }

private:
    bool m_Iconified;
};

// =============================================================================
// AppTickEvent — emitted once per frame before update (for instrumentation).
// =============================================================================
class AppTickEvent : public Event
{
public:
    EVENT_CLASS_TYPE(AppTick);
    EVENT_CLASS_CATEGORY(Application);

    const char* GetName() const override { return "AppTick"; }
};

// =============================================================================
// AppUpdateEvent — emitted once per frame during update phase.
// =============================================================================
class AppUpdateEvent : public Event
{
public:
    explicit AppUpdateEvent(double elapsedSeconds)
        : m_ElapsedSeconds(elapsedSeconds)
    {}

    double GetElapsedSeconds() const { return m_ElapsedSeconds; }

    EVENT_CLASS_TYPE(AppUpdate);
    EVENT_CLASS_CATEGORY(Application);

    const char* GetName() const override { return "AppUpdate"; }
    std::string ToString() const override
    {
        std::ostringstream ss;
        ss << "AppUpdate: dt=" << m_ElapsedSeconds;
        return ss.str();
    }

private:
    double m_ElapsedSeconds;
};

// =============================================================================
// AppRenderEvent — emitted once per frame during render phase.
// =============================================================================
class AppRenderEvent : public Event
{
public:
    EVENT_CLASS_TYPE(AppRender);
    EVENT_CLASS_CATEGORY(Application);

    const char* GetName() const override { return "AppRender"; }
};

} // namespace caustica

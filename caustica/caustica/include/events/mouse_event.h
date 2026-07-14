#pragma once

#include "events/event.h"
#include "events/key_codes.h"

#include <sstream>

namespace caustica
{

// =============================================================================
// MouseMovedEvent — cursor position change.
// =============================================================================
class MouseMovedEvent : public Event
{
public:
    MouseMovedEvent(double x, double y)
        : m_MouseX(x), m_MouseY(y)
    {}

    double getX() const { return m_MouseX; }
    double getY() const { return m_MouseY; }

    EVENT_CLASS_TYPE(MouseMoved);
    EVENT_CLASS_CATEGORY_FLAGS(EventCategory::Input | EventCategory::Mouse);

    const char* getName() const override { return "MouseMoved"; }
    std::string toString() const override
    {
        std::ostringstream ss;
        ss << "MouseMoved: x=" << m_MouseX << " y=" << m_MouseY;
        return ss.str();
    }

private:
    double m_MouseX;
    double m_MouseY;
};

// =============================================================================
// MouseScrolledEvent — scroll wheel movement.
// =============================================================================
class MouseScrolledEvent : public Event
{
public:
    MouseScrolledEvent(double xoffset, double yoffset)
        : m_XOffset(xoffset), m_YOffset(yoffset)
    {}

    double getXOffset() const { return m_XOffset; }
    double getYOffset() const { return m_YOffset; }

    EVENT_CLASS_TYPE(MouseScrolled);
    EVENT_CLASS_CATEGORY_FLAGS(EventCategory::Input | EventCategory::Mouse);

    const char* getName() const override { return "MouseScrolled"; }
    std::string toString() const override
    {
        std::ostringstream ss;
        ss << "MouseScrolled: dx=" << m_XOffset << " dy=" << m_YOffset;
        return ss.str();
    }

private:
    double m_XOffset;
    double m_YOffset;
};

// =============================================================================
// MouseButtonPressedEvent — mouse button pressed.
// =============================================================================
class MouseButtonPressedEvent : public Event
{
public:
    MouseButtonPressedEvent(MouseCode button, ModifierKey mods)
        : m_Button(button), m_Mods(mods)
    {}

    MouseCode   getButton()    const { return m_Button; }
    ModifierKey getModifiers() const { return m_Mods; }

    EVENT_CLASS_TYPE(MouseButtonPressed);
    EVENT_CLASS_CATEGORY_FLAGS(EventCategory::Input | EventCategory::Mouse | EventCategory::MouseButton);

    const char* getName() const override { return "MouseButtonPressed"; }
    std::string toString() const override
    {
        std::ostringstream ss;
        ss << "MouseButtonPressed: button=" << m_Button;
        return ss.str();
    }

private:
    MouseCode   m_Button;
    ModifierKey m_Mods;
};

// =============================================================================
// MouseButtonReleasedEvent — mouse button released.
// =============================================================================
class MouseButtonReleasedEvent : public Event
{
public:
    MouseButtonReleasedEvent(MouseCode button, ModifierKey mods)
        : m_Button(button), m_Mods(mods)
    {}

    MouseCode   getButton()    const { return m_Button; }
    ModifierKey getModifiers() const { return m_Mods; }

    EVENT_CLASS_TYPE(MouseButtonReleased);
    EVENT_CLASS_CATEGORY_FLAGS(EventCategory::Input | EventCategory::Mouse | EventCategory::MouseButton);

    const char* getName() const override { return "MouseButtonReleased"; }
    std::string toString() const override
    {
        std::ostringstream ss;
        ss << "MouseButtonReleased: button=" << m_Button;
        return ss.str();
    }

private:
    MouseCode   m_Button;
    ModifierKey m_Mods;
};

} // namespace caustica

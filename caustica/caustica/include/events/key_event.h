#pragma once

#include "events/event.h"
#include "events/key_codes.h"

#include <sstream>

namespace caustica
{

// =============================================================================
// KeyPressedEvent — sent when a keyboard key is pressed or repeated.
// =============================================================================
class KeyPressedEvent : public Event
{
public:
    KeyPressedEvent(KeyCode keyCode, int scancode, int repeatCount, ModifierKey mods)
        : m_KeyCode(keyCode), m_Scancode(scancode), m_RepeatCount(repeatCount), m_Mods(mods)
    {}

    KeyCode     getKeyCode()     const { return m_KeyCode; }
    int         getScancode()    const { return m_Scancode; }
    int         getRepeatCount() const { return m_RepeatCount; }
    ModifierKey getModifiers()   const { return m_Mods; }

    bool isRepeat() const { return m_RepeatCount > 0; }

    EVENT_CLASS_TYPE(KeyPressed);
    EVENT_CLASS_CATEGORY_FLAGS(EventCategory::Input | EventCategory::Keyboard);

    const char* getName() const override { return "KeyPressed"; }
    std::string toString() const override
    {
        std::ostringstream ss;
        ss << "KeyPressed: key=" << m_KeyCode << " repeat=" << m_RepeatCount;
        return ss.str();
    }

private:
    KeyCode     m_KeyCode;
    int         m_Scancode;
    int         m_RepeatCount;
    ModifierKey m_Mods;
};

// =============================================================================
// KeyReleasedEvent — sent when a keyboard key is released.
// =============================================================================
class KeyReleasedEvent : public Event
{
public:
    KeyReleasedEvent(KeyCode keyCode, int scancode, ModifierKey mods)
        : m_KeyCode(keyCode), m_Scancode(scancode), m_Mods(mods)
    {}

    KeyCode     getKeyCode()   const { return m_KeyCode; }
    int         getScancode()  const { return m_Scancode; }
    ModifierKey getModifiers() const { return m_Mods; }

    EVENT_CLASS_TYPE(KeyReleased);
    EVENT_CLASS_CATEGORY_FLAGS(EventCategory::Input | EventCategory::Keyboard);

    const char* getName() const override { return "KeyReleased"; }
    std::string toString() const override
    {
        std::ostringstream ss;
        ss << "KeyReleased: key=" << m_KeyCode;
        return ss.str();
    }

private:
    KeyCode     m_KeyCode;
    int         m_Scancode;
    ModifierKey m_Mods;
};

// =============================================================================
// KeyTypedEvent — sent when a character is typed (text input, not key codes).
// =============================================================================
class KeyTypedEvent : public Event
{
public:
    explicit KeyTypedEvent(unsigned int codepoint)
        : m_Codepoint(codepoint)
    {}

    unsigned int getCodepoint() const { return m_Codepoint; }

    EVENT_CLASS_TYPE(KeyTyped);
    EVENT_CLASS_CATEGORY_FLAGS(EventCategory::Input | EventCategory::Keyboard);

    const char* getName() const override { return "KeyTyped"; }
    std::string toString() const override
    {
        std::ostringstream ss;
        ss << "KeyTyped: codepoint=" << m_Codepoint;
        return ss.str();
    }

private:
    unsigned int m_Codepoint;
};

} // namespace caustica

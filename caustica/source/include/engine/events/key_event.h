/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*/

#pragma once

#include "engine/events/event.h"

namespace caustica
{

// Key action constants (matches GLFW)
enum class KeyAction
{
    Release = 0,
    Press   = 1,
    Repeat  = 2
};

class KeyEvent : public Event
{
public:
    DECLARE_EVENT_TYPE(KeyEvent, "Key")

    KeyEvent(int key, int scancode, int action, int mods)
        : m_Key(key), m_Scancode(scancode)
        , m_Action(action), m_Mods(mods) {}

    int getKey()      const { return m_Key; }
    int getScancode() const { return m_Scancode; }
    int getAction()   const { return m_Action; }
    int getMods()     const { return m_Mods; }

    bool isPressed()  const { return m_Action == static_cast<int>(KeyAction::Press); }
    bool isReleased() const { return m_Action == static_cast<int>(KeyAction::Release); }
    bool isRepeat()   const { return m_Action == static_cast<int>(KeyAction::Repeat); }

private:
    int m_Key;
    int m_Scancode;
    int m_Action;
    int m_Mods;
};

class CharInputEvent : public Event
{
public:
    DECLARE_EVENT_TYPE(CharInputEvent, "CharInput")

    explicit CharInputEvent(unsigned int codepoint) : m_Codepoint(codepoint) {}
    unsigned int getCodepoint() const { return m_Codepoint; }

private:
    unsigned int m_Codepoint;
};

} // namespace caustica

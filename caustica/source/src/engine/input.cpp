/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*/

#include "engine/input.h"

namespace caustica
{

Input* Input::s_Instance = nullptr;

Input& Input::get()
{
    if (!s_Instance)
    {
        static Input defaultInstance;
        s_Instance = &defaultInstance;
    }
    return *s_Instance;
}

void Input::release()
{
    s_Instance = nullptr;
}

void Input::resetPressed()
{
    // Clear edge flags: shift bit1→bit0, clear bit1
    for (auto& s : m_KeyState)
        s = (s & 1) ? 1 : 0;  // keep current down state as "was down"
    for (auto& s : m_MouseState)
        s = (s & 1) ? 1 : 0;

    m_LastMouseX  = m_MouseX;
    m_LastMouseY  = m_MouseY;
    m_MouseDeltaX = 0.0;
    m_MouseDeltaY = 0.0;
    m_ScrollX     = 0.0;
    m_ScrollY     = 0.0;
}

void Input::onEvent(Event& e)
{
    EventDispatcher dispatcher(e);

    dispatcher.dispatch<KeyEvent>([this](KeyEvent& ke)
    {
        if (ke.getKey() < 0 || static_cast<size_t>(ke.getKey()) >= kMaxKeys)
            return;
        if (ke.isPressed())
            m_KeyState[ke.getKey()] = 3;   // bits: currently down + just pressed
        else if (ke.isReleased())
            m_KeyState[ke.getKey()] = 0;   // not down
    });

    dispatcher.dispatch<MouseMoveEvent>([this](MouseMoveEvent& me)
    {
        m_MouseX      = me.getX();
        m_MouseY      = me.getY();
        m_MouseDeltaX = m_MouseX - m_LastMouseX;
        m_MouseDeltaY = m_MouseY - m_LastMouseY;
    });

    dispatcher.dispatch<MouseButtonEvent>([this](MouseButtonEvent& mb)
    {
        if (mb.getButton() < 0 || static_cast<size_t>(mb.getButton()) >= kMaxButtons)
            return;
        if (mb.isPressed())
            m_MouseState[mb.getButton()] = 3;
        else if (mb.isReleased())
            m_MouseState[mb.getButton()] = 0;
    });

    dispatcher.dispatch<MouseScrollEvent>([this](MouseScrollEvent& ms)
    {
        m_ScrollX = ms.getXOffset();
        m_ScrollY = ms.getYOffset();
    });
}

bool Input::isKeyDown(int key) const
{
    if (key < 0 || static_cast<size_t>(key) >= kMaxKeys) return false;
    return (m_KeyState[key] & 1) != 0;
}

bool Input::isKeyPressed(int key) const
{
    if (key < 0 || static_cast<size_t>(key) >= kMaxKeys) return false;
    return m_KeyState[key] == 3;  // both current and edge bits set
}

bool Input::isKeyReleased(int key) const
{
    if (key < 0 || static_cast<size_t>(key) >= kMaxKeys) return false;
    return m_KeyState[key] == 2;  // was down, now released
}

bool Input::isMouseButtonDown(int button) const
{
    if (button < 0 || static_cast<size_t>(button) >= kMaxButtons) return false;
    return (m_MouseState[button] & 1) != 0;
}

bool Input::isMouseButtonPressed(int button) const
{
    if (button < 0 || static_cast<size_t>(button) >= kMaxButtons) return false;
    return m_MouseState[button] == 3;
}

bool Input::isMouseButtonReleased(int button) const
{
    if (button < 0 || static_cast<size_t>(button) >= kMaxButtons) return false;
    return m_MouseState[button] == 2;
}

} // namespace caustica

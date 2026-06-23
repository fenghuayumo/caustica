/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*/

#pragma once

#include "engine/events/event.h"

namespace caustica
{

// Fired when the window close button is clicked
class WindowCloseEvent : public Event
{
public:
    DECLARE_EVENT_TYPE(WindowCloseEvent, "WindowClose")
};

// Fired when the window is resized
class WindowResizeEvent : public Event
{
public:
    DECLARE_EVENT_TYPE(WindowResizeEvent, "WindowResize")

    WindowResizeEvent(int width, int height)
        : m_Width(width), m_Height(height) {}

    int getWidth() const  { return m_Width; }
    int getHeight() const { return m_Height; }

private:
    int m_Width  = 0;
    int m_Height = 0;
};

// Fired when the window gains or loses focus
class WindowFocusEvent : public Event
{
public:
    DECLARE_EVENT_TYPE(WindowFocusEvent, "WindowFocus")

    explicit WindowFocusEvent(bool focused) : m_Focused(focused) {}
    bool isFocused() const { return m_Focused; }

private:
    bool m_Focused = true;
};

} // namespace caustica

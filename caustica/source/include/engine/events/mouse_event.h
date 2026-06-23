/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*/

#pragma once

#include "engine/events/event.h"

namespace caustica
{

class MouseMoveEvent : public Event
{
public:
    DECLARE_EVENT_TYPE(MouseMoveEvent, "MouseMove")

    MouseMoveEvent(double x, double y) : m_X(x), m_Y(y) {}
    double getX() const { return m_X; }
    double getY() const { return m_Y; }

private:
    double m_X = 0.0, m_Y = 0.0;
};

class MouseButtonEvent : public Event
{
public:
    DECLARE_EVENT_TYPE(MouseButtonEvent, "MouseButton")

    MouseButtonEvent(int button, int action, int mods)
        : m_Button(button), m_Action(action), m_Mods(mods) {}

    int getButton() const { return m_Button; }
    int getAction() const { return m_Action; }
    int getMods()   const { return m_Mods; }
    bool isPressed()  const { return m_Action == 1; }
    bool isReleased() const { return m_Action == 0; }

private:
    int m_Button;
    int m_Action;
    int m_Mods;
};

class MouseScrollEvent : public Event
{
public:
    DECLARE_EVENT_TYPE(MouseScrollEvent, "MouseScroll")

    MouseScrollEvent(double xOffset, double yOffset)
        : m_XOffset(xOffset), m_YOffset(yOffset) {}

    double getXOffset() const { return m_XOffset; }
    double getYOffset() const { return m_YOffset; }

private:
    double m_XOffset = 0.0, m_YOffset = 0.0;
};

} // namespace caustica

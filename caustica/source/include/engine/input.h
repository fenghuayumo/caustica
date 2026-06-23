/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*/

#pragma once

#include "engine/events/key_event.h"
#include "engine/events/mouse_event.h"

#include <array>
#include <cstdint>

namespace caustica
{

// Input manager — polls keyboard and mouse state each frame.
// Singleton, initialized once by Application.
class Input
{
public:
    static Input& get();
    static void   release();

    // Called each frame to reset transient state (pressed/released edges)
    void resetPressed();

    // Process an incoming event (from Window callbacks)
    void onEvent(Event& e);

    // Keyboard state
    bool isKeyDown(int key) const;
    bool isKeyPressed(int key) const;   // edge: just pressed this frame
    bool isKeyReleased(int key) const;  // edge: just released this frame

    // Mouse state
    bool isMouseButtonDown(int button) const;
    bool isMouseButtonPressed(int button) const;
    bool isMouseButtonReleased(int button) const;

    double getMouseX() const { return m_MouseX; }
    double getMouseY() const { return m_MouseY; }
    double getMouseDeltaX() const { return m_MouseDeltaX; }
    double getMouseDeltaY() const { return m_MouseDeltaY; }
    double getScrollX() const { return m_ScrollX; }
    double getScrollY() const { return m_ScrollY; }

private:
    Input()  = default;
    ~Input() = default;

    static constexpr size_t kMaxKeys    = 512;
    static constexpr size_t kMaxButtons = 8;

    // Key state: bit 0 = current down, bit 1 = was down last frame
    std::array<uint8_t, kMaxKeys>    m_KeyState{};
    std::array<uint8_t, kMaxButtons> m_MouseState{};

    double m_MouseX      = 0.0;
    double m_MouseY      = 0.0;
    double m_LastMouseX  = 0.0;
    double m_LastMouseY  = 0.0;
    double m_MouseDeltaX = 0.0;
    double m_MouseDeltaY = 0.0;
    double m_ScrollX     = 0.0;
    double m_ScrollY     = 0.0;

    static Input* s_Instance;
};

} // namespace caustica

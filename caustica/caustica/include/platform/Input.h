#pragma once

#include <list>

namespace caustica {

// =============================================================================
// IInputHandler — Platform layer: interface for objects that receive input
// events (keyboard, mouse, joystick).
//
// Separated from IRenderPass. Render passes that need input implement this
// in addition to IRenderPass.
// =============================================================================
class IInputHandler
{
public:
    virtual ~IInputHandler() = default;

    virtual bool onKeyEvent(int key, int scancode, int action, int mods) { return false; }
    virtual bool onCharEvent(unsigned int codepoint, int mods)            { return false; }
    virtual bool onMouseMoveEvent(double xpos, double ypos)              { return false; }
    virtual bool onMouseButtonEvent(int button, int action, int mods)   { return false; }
    virtual bool onMouseScrollEvent(double xoffset, double yoffset)      { return false; }
    virtual bool onJoystickButtonEvent(int button, bool pressed)          { return false; }
    virtual bool onJoystickAxisEvent(int axis, float value)              { return false; }
};

// =============================================================================
// Input — Platform layer: aggregates input from GLFW callbacks and dispatches
// to registered IInputHandler instances.
// =============================================================================
class Input
{
public:
    Input() = default;
    ~Input() = default;

    // --- Registration ---
    void registerHandler(IInputHandler* handler);
    void unregisterHandler(IInputHandler* handler);

    // --- GLFW callback handlers ---
    void onKey(int key, int scancode, int action, int mods);
    void onChar(unsigned int codepoint, int mods);
    void onMouseMove(double xpos, double ypos);
    void onMouseButton(int button, int action, int mods);
    void onMouseScroll(double xoffset, double yoffset);

    // --- Joystick polling ---
    void pollJoysticks();

    // Joystick connection events (static relay from GLFW callback)
    static void onJoystickEvent(int joyId, int event);

    // --- GLFW callback installation ---
    void installCallbacks(void* glfwWindow);

private:
    std::list<IInputHandler*> m_Handlers;
};

} // namespace caustica

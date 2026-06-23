#pragma once

#include <list>
#include <functional>

namespace caustica {

class IRenderPass;

// =============================================================================
// Input — Engine layer: aggregates keyboard, mouse, and joystick input from
// GLFW callbacks and dispatches to registered IRenderPass handlers.
//
// Owned by Application. DeviceManager delegates input dispatch to this class.
// =============================================================================
class Input
{
public:
    Input() = default;
    ~Input() = default;

    // --- Registration ---
    void registerPass(IRenderPass* pass);
    void unregisterPass(IRenderPass* pass);

    // --- GLFW callback handlers ---
    void onKey(int key, int scancode, int action, int mods);
    void onChar(unsigned int codepoint, int mods);
    void onMouseMove(double xpos, double ypos);
    void onMouseButton(int button, int action, int mods);
    void onMouseScroll(double xoffset, double yoffset);

    // --- Joystick polling (called each frame by Application) ---
    void pollJoysticks();

    // Joystick connection events from GLFW callback (static relay)
    static void onJoystickEvent(int joyId, int event);

    // --- GLFW callback installation on a window ---
    void installCallbacks(void* glfwWindow);

private:
    std::list<IRenderPass*> m_Passes;
};

} // namespace caustica

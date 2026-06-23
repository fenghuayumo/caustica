#include "platform/Input.h"
#include "engine/DeviceManager.h"  // IRenderPass (for legacy compat in JoystickManager)
#include "math/vector.h"

#include <GLFW/glfw3.h>
#include <algorithm>

namespace caustica {

namespace {

class JoystickManager
{
public:
    static JoystickManager& get() { static JoystickManager i; return i; }

    void enumerate() {
        for (int j = 0; j < 10; ++j)
            if (glfwJoystickPresent(j)) m_IDs.push_back(j);
    }
    void eraseDisconnected() {
        while (!m_Removed.empty()) {
            auto id = m_Removed.back(); m_Removed.pop_back();
            auto it = std::find(m_IDs.begin(), m_IDs.end(), id);
            if (it != m_IDs.end()) m_IDs.erase(it);
        }
    }
    void connect(int id)    { m_IDs.push_back(id); }
    void disconnect(int id) { m_Removed.push_back(id); }

    void updateAll(const std::list<IInputHandler*>& handlers) {
        for (auto j : m_IDs) updateJoystick(j, handlers);
    }

private:
    JoystickManager() = default;

    static void applyDeadZone(dm::float2& v, float dz = 0.1f) {
        float len = dm::length(v);
        v *= (len > dz) ? ((len - dz) / (1.f - dz)) : 0.f;
    }

    void updateJoystick(int j, const std::list<IInputHandler*>& handlers) {
        GLFWgamepadstate gs;
        if (!glfwGetGamepadState(j, &gs)) return;
        float* ax = gs.axes;

        auto updateAxis = [&](int a, float v) {
            for (auto it = handlers.crbegin(); it != handlers.crend(); ++it)
                if ((*it)->onJoystickAxisEvent(a, v)) break;
        };
        auto updateButton = [&](int b, bool p) {
            for (auto it = handlers.crbegin(); it != handlers.crend(); ++it)
                if ((*it)->onJoystickButtonEvent(b, p)) break;
        };

        { dm::float2 v(ax[GLFW_GAMEPAD_AXIS_LEFT_X], ax[GLFW_GAMEPAD_AXIS_LEFT_Y]); applyDeadZone(v);
          updateAxis(GLFW_GAMEPAD_AXIS_LEFT_X, v.x); updateAxis(GLFW_GAMEPAD_AXIS_LEFT_Y, v.y); }
        { dm::float2 v(ax[GLFW_GAMEPAD_AXIS_RIGHT_X], ax[GLFW_GAMEPAD_AXIS_RIGHT_Y]); applyDeadZone(v);
          updateAxis(GLFW_GAMEPAD_AXIS_RIGHT_X, v.x); updateAxis(GLFW_GAMEPAD_AXIS_RIGHT_Y, v.y); }
        updateAxis(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER,  ax[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]);
        updateAxis(GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER, ax[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER]);
        for (int b = GLFW_GAMEPAD_BUTTON_A; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b)
            updateButton(b, gs.buttons[b] == GLFW_PRESS);
    }

    std::list<int> m_IDs, m_Removed;
};

static Input* s_Input = nullptr;

static void glfwKeyCb(GLFWwindow*, int k, int s, int a, int m) { if(s_Input) s_Input->onKey(k,s,a,m); }
static void glfwCharCb(GLFWwindow*, unsigned int c, int m)     { if(s_Input) s_Input->onChar(c,m); }
static void glfwCursorCb(GLFWwindow*, double x, double y)      { if(s_Input) s_Input->onMouseMove(x,y); }
static void glfwMouseCb(GLFWwindow*, int b, int a, int m)      { if(s_Input) s_Input->onMouseButton(b,a,m); }
static void glfwScrollCb(GLFWwindow*, double x, double y)      { if(s_Input) s_Input->onMouseScroll(x,y); }
static void glfwJoyCb(int j, int e) { Input::onJoystickEvent(j, e); }

} // anonymous namespace

// =============================================================================
// Input
// =============================================================================

void Input::registerHandler(IInputHandler* h) { m_Handlers.remove(h); m_Handlers.push_back(h); }
void Input::unregisterHandler(IInputHandler* h) { m_Handlers.remove(h); }

void Input::onKey(int key, int scancode, int action, int mods) {
    if (key == -1) return;
    for (auto it = m_Handlers.crbegin(); it != m_Handlers.crend(); ++it)
        if ((*it)->onKeyEvent(key, scancode, action, mods)) break;
}
void Input::onChar(unsigned int cp, int mods) {
    for (auto it = m_Handlers.crbegin(); it != m_Handlers.crend(); ++it)
        if ((*it)->onCharEvent(cp, mods)) break;
}
void Input::onMouseMove(double x, double y) {
    for (auto it = m_Handlers.crbegin(); it != m_Handlers.crend(); ++it)
        if ((*it)->onMouseMoveEvent(x, y)) break;
}
void Input::onMouseButton(int b, int a, int m) {
    for (auto it = m_Handlers.crbegin(); it != m_Handlers.crend(); ++it)
        if ((*it)->onMouseButtonEvent(b, a, m)) break;
}
void Input::onMouseScroll(double x, double y) {
    for (auto it = m_Handlers.crbegin(); it != m_Handlers.crend(); ++it)
        if ((*it)->onMouseScrollEvent(x, y)) break;
}
void Input::pollJoysticks() {
    JoystickManager::get().eraseDisconnected();
    JoystickManager::get().updateAll(m_Handlers);
}
void Input::onJoystickEvent(int j, int e) {
    if (e == GLFW_CONNECTED) JoystickManager::get().connect(j);
    else                     JoystickManager::get().disconnect(j);
}
void Input::installCallbacks(void* w) {
    auto* win = static_cast<GLFWwindow*>(w);
    s_Input = this;
    glfwSetKeyCallback(win, glfwKeyCb);
    glfwSetCharModsCallback(win, glfwCharCb);
    glfwSetCursorPosCallback(win, glfwCursorCb);
    glfwSetMouseButtonCallback(win, glfwMouseCb);
    glfwSetScrollCallback(win, glfwScrollCb);
    glfwSetJoystickCallback(glfwJoyCb);
    JoystickManager::get().enumerate();
}

} // namespace caustica

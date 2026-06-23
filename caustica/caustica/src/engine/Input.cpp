#include "engine/Input.h"
#include "engine/DeviceManager.h"  // IRenderPass definition
#include "math/vector.h"

#include <GLFW/glfw3.h>
#include <algorithm>

namespace caustica {

// =============================================================================
// JoystickManager — internal singleton (was global in DeviceManager.cpp)
// =============================================================================
namespace {

class JoystickManager
{
public:
    static JoystickManager& get()
    {
        static JoystickManager instance;
        return instance;
    }

    void enumerate()
    {
        for (int i = 0; i < 10; ++i)
            if (glfwJoystickPresent(i))
                m_JoystickIDs.push_back(i);
    }

    void eraseDisconnected()
    {
        while (!m_RemovedJoysticks.empty())
        {
            auto id = m_RemovedJoysticks.back();
            m_RemovedJoysticks.pop_back();
            auto it = std::find(m_JoystickIDs.begin(), m_JoystickIDs.end(), id);
            if (it != m_JoystickIDs.end())
                m_JoystickIDs.erase(it);
        }
    }

    void connect(int id)     { m_JoystickIDs.push_back(id); }
    void disconnect(int id)  { m_RemovedJoysticks.push_back(id); }

    void updateAll(const std::list<IRenderPass*>& passes)
    {
        for (auto j : m_JoystickIDs)
            updateJoystick(j, passes);
    }

private:
    JoystickManager() = default;

    static void applyDeadZone(dm::float2& v, float deadZone = 0.1f)
    {
        float len = dm::length(v);
        v *= (len > deadZone) ? ((len - deadZone) / (1.f - deadZone)) : 0.f;
    }

    void updateJoystick(int j, const std::list<IRenderPass*>& passes)
    {
        GLFWgamepadstate gamepadState;
        if (!glfwGetGamepadState(j, &gamepadState))
            return;

        float* axisValues = gamepadState.axes;

        auto updateAxis = [&](int axis, float val) {
            for (auto it = passes.crbegin(); it != passes.crend(); ++it)
                if ((*it)->JoystickAxisUpdate(axis, val))
                    break;
        };

        // Left stick
        {
            dm::float2 v(axisValues[GLFW_GAMEPAD_AXIS_LEFT_X], axisValues[GLFW_GAMEPAD_AXIS_LEFT_Y]);
            applyDeadZone(v);
            updateAxis(GLFW_GAMEPAD_AXIS_LEFT_X, v.x);
            updateAxis(GLFW_GAMEPAD_AXIS_LEFT_Y, v.y);
        }

        // Right stick
        {
            dm::float2 v(axisValues[GLFW_GAMEPAD_AXIS_RIGHT_X], axisValues[GLFW_GAMEPAD_AXIS_RIGHT_Y]);
            applyDeadZone(v);
            updateAxis(GLFW_GAMEPAD_AXIS_RIGHT_X, v.x);
            updateAxis(GLFW_GAMEPAD_AXIS_RIGHT_Y, v.y);
        }

        // Triggers
        updateAxis(GLFW_GAMEPAD_AXIS_LEFT_TRIGGER,  axisValues[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]);
        updateAxis(GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER, axisValues[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER]);

        // Buttons
        for (int b = GLFW_GAMEPAD_BUTTON_A; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b)
        {
            bool pressed = (gamepadState.buttons[b] == GLFW_PRESS);
            for (auto it = passes.crbegin(); it != passes.crend(); ++it)
                if ((*it)->JoystickButtonUpdate(b, pressed))
                    break;
        }
    }

    std::list<int> m_JoystickIDs;
    std::list<int> m_RemovedJoysticks;
};

// GLFW input callbacks — use window user pointer (set to Input* by installCallbacks)
static Input* s_InputForCallbacks = nullptr;

static void glfwKeyCallback(GLFWwindow*, int key, int scancode, int action, int mods)
{
    if (s_InputForCallbacks) s_InputForCallbacks->onKey(key, scancode, action, mods);
}
static void glfwCharModsCallback(GLFWwindow*, unsigned int codepoint, int mods)
{
    if (s_InputForCallbacks) s_InputForCallbacks->onChar(codepoint, mods);
}
static void glfwCursorPosCallback(GLFWwindow*, double xpos, double ypos)
{
    if (s_InputForCallbacks) s_InputForCallbacks->onMouseMove(xpos, ypos);
}
static void glfwMouseButtonCallback(GLFWwindow*, int button, int action, int mods)
{
    if (s_InputForCallbacks) s_InputForCallbacks->onMouseButton(button, action, mods);
}
static void glfwScrollCallback(GLFWwindow*, double xoffset, double yoffset)
{
    if (s_InputForCallbacks) s_InputForCallbacks->onMouseScroll(xoffset, yoffset);
}
static void glfwJoystickCallback(int joyId, int event)
{
    Input::onJoystickEvent(joyId, event);
}

} // anonymous namespace

// =============================================================================
// Input
// =============================================================================

void Input::registerPass(IRenderPass* pass)
{
    m_Passes.remove(pass);
    m_Passes.push_back(pass);
}

void Input::unregisterPass(IRenderPass* pass)
{
    m_Passes.remove(pass);
}

void Input::onKey(int key, int scancode, int action, int mods)
{
    if (key == -1) return;
    for (auto it = m_Passes.crbegin(); it != m_Passes.crend(); ++it)
    {
        if ((*it)->KeyboardUpdate(key, scancode, action, mods))
            break;
    }
}

void Input::onChar(unsigned int codepoint, int mods)
{
    for (auto it = m_Passes.crbegin(); it != m_Passes.crend(); ++it)
    {
        if ((*it)->KeyboardCharInput(codepoint, mods))
            break;
    }
}

void Input::onMouseMove(double xpos, double ypos)
{
    for (auto it = m_Passes.crbegin(); it != m_Passes.crend(); ++it)
    {
        if ((*it)->MousePosUpdate(xpos, ypos))
            break;
    }
}

void Input::onMouseButton(int button, int action, int mods)
{
    for (auto it = m_Passes.crbegin(); it != m_Passes.crend(); ++it)
    {
        if ((*it)->MouseButtonUpdate(button, action, mods))
            break;
    }
}

void Input::onMouseScroll(double xoffset, double yoffset)
{
    for (auto it = m_Passes.crbegin(); it != m_Passes.crend(); ++it)
    {
        if ((*it)->MouseScrollUpdate(xoffset, yoffset))
            break;
    }
}

void Input::pollJoysticks()
{
    JoystickManager::get().eraseDisconnected();
    JoystickManager::get().updateAll(m_Passes);
}

void Input::onJoystickEvent(int joyId, int event)
{
    if (event == GLFW_CONNECTED)
        JoystickManager::get().connect(joyId);
    else
        JoystickManager::get().disconnect(joyId);
}

void Input::installCallbacks(void* glfwWindow)
{
    auto* window = static_cast<GLFWwindow*>(glfwWindow);
    s_InputForCallbacks = this;

    glfwSetKeyCallback(window, glfwKeyCallback);
    glfwSetCharModsCallback(window, glfwCharModsCallback);
    glfwSetCursorPosCallback(window, glfwCursorPosCallback);
    glfwSetMouseButtonCallback(window, glfwMouseButtonCallback);
    glfwSetScrollCallback(window, glfwScrollCallback);
    glfwSetJoystickCallback(glfwJoystickCallback);

    JoystickManager::get().enumerate();
}

} // namespace caustica

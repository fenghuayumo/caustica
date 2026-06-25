#include "input/PathTracerInputController.h"

#include "EditorUIState.h"
#include <render/Core/PathTracerSettings.h>
#include <render/Core/RenderCore.h>
#include <render/Passes/Debug/ZoomTool.h>
#include <backend/GpuDevice.h>
#include <scene/camera/Camera.h>

#include <events/key_event.h>
#include <events/mouse_event.h>

#include "SampleGame/GameScene.h"

#include <imgui.h>

// KeyCode/MouseCode values match GLFW 1:1, so casting back to int is zero-cost.
// This keeps Camera/ZoomTool/GameScene working unchanged until they're migrated
// to use the event types directly.
namespace
{
inline constexpr int ToGlfwKey(caustica::KeyCode k)   { return static_cast<int>(k); }
inline constexpr int ToGlfwMouse(caustica::MouseCode m) { return static_cast<int>(m); }
inline constexpr int ToGlfwMods(caustica::ModifierKey m) { return static_cast<int>(m); }

// GLFW action codes (used by Camera/ZoomTool/GameScene)
inline constexpr int cGlfwPress   = 1;  // GLFW_PRESS
inline constexpr int cGlfwRelease = 0;  // GLFW_RELEASE
inline constexpr int cGlfwRepeat  = 2;  // GLFW_REPEAT
}

#if CAUSTICA_WITH_STREAMLINE
#include <engine/StreamlineInterface.h>
#endif

PathTracerInputController::PathTracerInputController(Bindings bindings)
    : m_bindings(std::move(bindings))
{
}

void PathTracerInputController::onEvent(caustica::Event& event)
{
    caustica::EventDispatcher dispatcher(event);

    dispatcher.Dispatch<caustica::KeyPressedEvent>(
        [this](auto& e) { return onKeyPressed(e); });
    dispatcher.Dispatch<caustica::KeyReleasedEvent>(
        [this](auto& e) { return onKeyReleased(e); });
    dispatcher.Dispatch<caustica::MouseMovedEvent>(
        [this](auto& e) { return onMouseMoved(e); });
    dispatcher.Dispatch<caustica::MouseButtonPressedEvent>(
        [this](auto& e) { return onMouseButtonPressed(e); });
    dispatcher.Dispatch<caustica::MouseButtonReleasedEvent>(
        [this](auto& e) { return onMouseButtonReleased(e); });
    dispatcher.Dispatch<caustica::MouseScrolledEvent>(
        [this](auto& e) { return onMouseScrolled(e); });
}

bool PathTracerInputController::onKeyPressed(caustica::KeyPressedEvent& e)
{
    int key   = ToGlfwKey(e.GetKeyCode());
    int mods  = ToGlfwMods(e.GetModifiers());
    int action = e.IsRepeat() ? cGlfwRepeat : cGlfwPress;

    ZoomTool* zoomTool = m_bindings.getZoomTool ? m_bindings.getZoomTool() : nullptr;
    if (zoomTool && zoomTool->KeyboardUpdate(key, e.GetScancode(), action, mods))
        return true;

    GameScene* game = m_bindings.sampleGame;
    if (!(game && game->CameraActive()))
        m_bindings.renderCore.camera().camera().KeyboardUpdate(key, e.GetScancode(), action, mods);

    if (game && game->KeyboardUpdate(key, e.GetScancode(), action, mods))
        return true;

    if (key == ToGlfwKey(caustica::Key::Space) && action == cGlfwPress && mods != ToGlfwMods(caustica::ModifierKey::Control) && mods != ToGlfwMods(caustica::ModifierKey::Alt))
    {
        m_bindings.settings.EnableAnimations = !m_bindings.settings.EnableAnimations;
        return true;
    }
    if (key == ToGlfwKey(caustica::Key::F2) && action == cGlfwPress)
        m_bindings.editor.ShowUI = !m_bindings.editor.ShowUI;
    if (key == ToGlfwKey(caustica::Key::R) && action == cGlfwPress && mods == ToGlfwMods(caustica::ModifierKey::Control))
        m_bindings.editor.ShaderReloadRequested = true;

#if CAUSTICA_WITH_STREAMLINE
    if (key == ToGlfwKey(caustica::Key::F13) && action == cGlfwPress)
        m_bindings.gpuDevice.GetStreamline().ReflexTriggerPcPing(m_bindings.gpuDevice.GetFrameIndex());
#endif

    return true;
}

bool PathTracerInputController::onKeyReleased(caustica::KeyReleasedEvent& e)
{
    int key   = ToGlfwKey(e.GetKeyCode());
    int mods  = ToGlfwMods(e.GetModifiers());

    ZoomTool* zoomTool = m_bindings.getZoomTool ? m_bindings.getZoomTool() : nullptr;
    if (zoomTool && zoomTool->KeyboardUpdate(key, e.GetScancode(), cGlfwRelease, mods))
        return true;

    GameScene* game = m_bindings.sampleGame;
    if (!(game && game->CameraActive()))
        m_bindings.renderCore.camera().camera().KeyboardUpdate(key, e.GetScancode(), cGlfwRelease, mods);

    if (game && game->KeyboardUpdate(key, e.GetScancode(), cGlfwRelease, mods))
        return true;

    return true;
}

bool PathTracerInputController::onMouseMoved(caustica::MouseMovedEvent& e)
{
    double xpos = e.GetX();
    double ypos = e.GetY();

    if (ImGui::GetIO().WantCaptureMouse)
        return false;

    GameScene* game = m_bindings.sampleGame;
    if (!(game && game->CameraActive()))
        m_bindings.renderCore.camera().camera().MousePosUpdate(xpos, ypos);
    if (game)
        game->MousePosUpdate(xpos, ypos);

    const dm::float2 upscalingScale = m_bindings.getUpscalingScale ? m_bindings.getUpscalingScale() : dm::float2(1.0f, 1.0f);
    const dm::uint2 renderPos{
        static_cast<uint>(xpos * upscalingScale.x),
        static_cast<uint>(ypos * upscalingScale.y),
    };

    m_bindings.editor.PickPosition = renderPos;
    m_bindings.settings.MousePos = renderPos;

    if (ZoomTool* zoomTool = m_bindings.getZoomTool ? m_bindings.getZoomTool() : nullptr)
        zoomTool->MousePosUpdate(xpos, ypos);

    return true;
}

bool PathTracerInputController::onMouseButtonPressed(caustica::MouseButtonPressedEvent& e)
{
    int button = ToGlfwMouse(e.GetButton());
    int mods   = ToGlfwMods(e.GetModifiers());

    if (ImGui::GetIO().WantCaptureMouse)
        return false;

    if (ZoomTool* zoomTool = m_bindings.getZoomTool ? m_bindings.getZoomTool() : nullptr)
        if (zoomTool->MouseButtonUpdate(button, cGlfwPress, mods))
            return true;

    GameScene* game = m_bindings.sampleGame;
    if (!(game && game->CameraActive()))
        m_bindings.renderCore.camera().camera().MouseButtonUpdate(button, cGlfwPress, mods);
    if (game)
        game->MouseButtonUpdate(button, cGlfwPress, mods);

    if (button == ToGlfwMouse(caustica::Mouse::Right))
    {
        m_bindings.editor.PickMaterialRequested = true;
        m_bindings.editor.PickInstanceRequested = true;
        m_bindings.settings.DebugPixel = m_bindings.editor.PickPosition;
    }

#if CAUSTICA_WITH_STREAMLINE
    if (button == ToGlfwMouse(caustica::Mouse::Left))
        m_bindings.gpuDevice.GetStreamline().ReflexTriggerFlash(m_bindings.gpuDevice.GetFrameIndex());
#endif

    return true;
}

bool PathTracerInputController::onMouseButtonReleased(caustica::MouseButtonReleasedEvent& e)
{
    int button = ToGlfwMouse(e.GetButton());
    int mods   = ToGlfwMods(e.GetModifiers());

    if (ImGui::GetIO().WantCaptureMouse)
        return false;

    if (ZoomTool* zoomTool = m_bindings.getZoomTool ? m_bindings.getZoomTool() : nullptr)
        if (zoomTool->MouseButtonUpdate(button, cGlfwRelease, mods))
            return true;

    GameScene* game = m_bindings.sampleGame;
    if (!(game && game->CameraActive()))
        m_bindings.renderCore.camera().camera().MouseButtonUpdate(button, cGlfwRelease, mods);
    if (game)
        game->MouseButtonUpdate(button, cGlfwRelease, mods);

    return true;
}

bool PathTracerInputController::onMouseScrolled(caustica::MouseScrolledEvent& e)
{
    double xoffset = e.GetXOffset();
    double yoffset = e.GetYOffset();

    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseWheelEvent(static_cast<float>(xoffset), static_cast<float>(yoffset));

    if (io.WantCaptureMouse)
        return true;

    GameScene* game = m_bindings.sampleGame;
    if (!(game && game->CameraActive()))
        m_bindings.settings.CameraMoveSpeed *= 1.0f + static_cast<float>(yoffset) * 0.1f;

    return true;
}

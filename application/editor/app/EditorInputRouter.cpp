#include <algorithm>
#include <cmath>
#include "EditorInputRouter.h"

#include "SceneEditor.h"
#include "EditorAccess.h"
#include "EditorUIState.h"

#include <backend/GpuDevice.h>
#include <core/log.h>
#include <ecs/Entity.h>
#include <events/event.h>
#include <events/key_event.h>
#include <events/mouse_event.h>
#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui/imgui_renderer.h>
#include <render/RenderAppState.h>
#include <render/WorldRenderer.h>
#include "game/GameScene.h"
#include <render/passes/debug/ZoomTool.h>

namespace caustica::editor
{

namespace
{
bool uiCapturesMouseForEditor(const SceneEditor& sceneEditor)
{
    // Viewport canvas is an ImGui item, but camera/gizmo must still receive mouse there.
    if (!ImGui::GetIO().WantCaptureMouse)
        return false;
    if (sceneEditor.editorUIState().Viewport.Hovered)
        return false;
    return true;
}

inline constexpr int ToGlfwKey(caustica::KeyCode k) { return static_cast<int>(k); }
inline constexpr int ToGlfwMouse(caustica::MouseCode m) { return static_cast<int>(m); }
inline constexpr int ToGlfwMods(caustica::ModifierKey m) { return static_cast<int>(m); }
inline constexpr int cGlfwPress = 1;
inline constexpr int cGlfwRelease = 0;
inline constexpr int cGlfwRepeat = 2;

// WASD/QE/ZC only apply while RMB is held (fly mode). Arrow keys stay free for pan.
bool isCameraFlyKey(int key)
{
    return key == ToGlfwKey(caustica::Key::W)
        || key == ToGlfwKey(caustica::Key::A)
        || key == ToGlfwKey(caustica::Key::S)
        || key == ToGlfwKey(caustica::Key::D)
        || key == ToGlfwKey(caustica::Key::Q)
        || key == ToGlfwKey(caustica::Key::E)
        || key == ToGlfwKey(caustica::Key::Z)
        || key == ToGlfwKey(caustica::Key::C);
}

bool isRightMouseDown(SceneEditor& sceneEditor)
{
    GLFWwindow* window = sceneEditor.app() && sceneEditor.app()->getGpuDevice()
        ? sceneEditor.app()->getGpuDevice()->getWindow()
        : nullptr;
    return window && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
}

struct RightClickPickState
{
    bool tracking = false;
    bool dragged = false;
    double pressX = 0.0;
    double pressY = 0.0;
};

RightClickPickState g_rightClickPick;
constexpr double kRightClickPickSlopPx = 4.0;

bool gizmoCapturesInput(const SceneEditor& sceneEditor)
{
    const auto& editor = sceneEditor.editorUIState();
    return editor.GizmoCapturingInput || ImGuizmo::IsOver() || ImGuizmo::IsUsing();
}

void requestMaterialPick(caustica::render::RenderRuntimeState& runtime)
{
    // Right-click: pick the hit geometry's material (per sub-mesh), not the whole instance.
    runtime.Picking.MaterialRequested = true;
}

void requestInstancePick(caustica::render::RenderRuntimeState& runtime)
{
    // Left-click: select the mesh instance entity for Inspector / gizmo.
    runtime.Picking.InstanceRequested = true;
}

void syncPickPositionFromCursor(SceneEditor& sceneEditor)
{
    auto& session = sceneEditor.renderAppState();
    GLFWwindow* window = sceneEditor.app()->getGpuDevice()->getWindow();
    if (!window)
        return;

    double cursorX = 0.0;
    double cursorY = 0.0;
    glfwGetCursorPos(window, &cursorX, &cursorY);

    // Map window cursor into viewport-local display pixels when the true viewport is active.
    // Path-trace pick pixels are derived later from this frame's settled renderSize (after DLSS).
    const auto& vp = sceneEditor.editorUIState().Viewport;
    double pickX = cursorX;
    double pickY = cursorY;
    if (vp.RectValid && vp.SizeX > 1.f && vp.SizeY > 1.f)
    {
        pickX = cursorX - static_cast<double>(vp.PosX);
        pickY = cursorY - static_cast<double>(vp.PosY);
        pickX = std::clamp(pickX, 0.0, static_cast<double>(vp.SizeX) - 1.0);
        pickY = std::clamp(pickY, 0.0, static_cast<double>(vp.SizeY) - 1.0);
    }

    session.runtime.Picking.Position = dm::uint2{
        static_cast<uint>(pickX),
        static_cast<uint>(pickY)};
    session.settings.MousePos = session.runtime.Picking.Position;
    session.settings.DebugPixel = session.runtime.Picking.Position;
}

bool onKeyPressed(SceneEditor& sceneEditor, caustica::KeyPressedEvent& e)
{
    const int key = ToGlfwKey(e.getKeyCode());
    const int mods = ToGlfwMods(e.getModifiers());
    const int action = e.isRepeat() ? cGlfwRepeat : cGlfwPress;

    imGuiForwardKeyboard(key, action, e.getScancode());

    const bool ctrlDown = (mods & ToGlfwMods(caustica::ModifierKey::Control)) != 0;
    const bool shiftDown = (mods & ToGlfwMods(caustica::ModifierKey::Shift)) != 0;
    const bool altDown = (mods & ToGlfwMods(caustica::ModifierKey::Alt)) != 0;

    // Editor undo/redo: queue for after EditorUI/gizmo commit (see processPendingEditActions).
    // Handle even when ImGui wants keyboard, but never steal text-field undo.
    if (action == cGlfwPress && ctrlDown && !altDown && !ImGui::GetIO().WantTextInput)
    {
        if (key == ToGlfwKey(caustica::Key::Z) && !shiftDown)
        {
            sceneEditor.requestUndo();
            return true;
        }
        if ((key == ToGlfwKey(caustica::Key::Y) && !shiftDown)
            || (key == ToGlfwKey(caustica::Key::Z) && shiftDown))
        {
            sceneEditor.requestRedo();
            return true;
        }
        if (key == ToGlfwKey(caustica::Key::O) && !shiftDown)
        {
            sceneEditor.requestOpenSceneFromDialog();
            return true;
        }
        if (key == ToGlfwKey(caustica::Key::S) && !shiftDown)
        {
            sceneEditor.requestSaveScene();
            return true;
        }
    }

    // I / Shift+I: insert keyframe at current Timeline frame (Blender-style).
    // Works while panels capture keyboard; skip when typing in a text field.
    if (action == cGlfwPress && !ctrlDown && !altDown && !ImGui::GetIO().WantTextInput
        && key == ToGlfwKey(caustica::Key::I))
    {
        auto& editor = sceneEditor.editorUIState();
        const ecs::Entity selected = editor.SelectedEntity;
        if (ecs::isValid(selected))
        {
            const int fps = std::max(1, editor.FramesPerSecond);
            const float frameSeconds = 1.f / static_cast<float>(fps);
            float displayTime = static_cast<float>(sceneEditor.timelineTime());
            const float duration = sceneEditor.animationDuration();
            if (duration > 0.f && displayTime > duration)
                displayTime = std::fmod(displayTime, duration);
            const int currentFrame = std::clamp(
                static_cast<int>(std::lround(displayTime * fps)),
                editor.StartFrame,
                editor.EndFrame);
            const float keyTime = static_cast<float>(currentFrame) * frameSeconds;

            sceneEditor.renderAppState().settings.EnableKeyframes = false;
            if (shiftDown)
            {
                if (sceneEditor.canAnimateVisibility(selected))
                    sceneEditor.insertVisibilityKeyframe(selected, keyTime);
            }
            else
            {
                sceneEditor.insertTransformKeyframe(selected, keyTime);
            }
        }
        return true;
    }

    // UE-style command bar: ` / ~ (Esc 下方). Always toggle — do not require leaving
    // text focus first. Also accept the common US/Win scancode for IME remaps.
    constexpr int kGraveScancode = 0x29;
    const bool graveKey = key == ToGlfwKey(caustica::Key::GraveAccent)
        || key == ToGlfwKey(caustica::Key::World1)
        || e.getScancode() == kGraveScancode;
    if (graveKey && action == cGlfwPress && !ctrlDown && !altDown)
    {
        auto& editor = sceneEditor.editorUIState();
        editor.ShowCommandBar = !editor.ShowCommandBar;
        if (editor.ShowCommandBar)
            editor.RequestFocusCommandBar = true;
        return true;
    }

    if (ImGui::GetIO().WantCaptureKeyboard)
        return true;

    auto* camera = caustica::editor::editorCamera(sceneEditor);
    if (!camera)
        return true;

    auto* zoomTool = sceneEditor.zoomTool().get();
    auto* game = sceneEditor.game().get();

    if (zoomTool && zoomTool->keyboardUpdate(key, e.getScancode(), action, mods))
        return true;

    // WASD/QE fly only while RMB is held so Q/T/R/S gizmo hotkeys stay free.
    if (!(game && game->CameraActive()))
    {
        if (!isCameraFlyKey(key) || isRightMouseDown(sceneEditor))
            camera->camera().keyboardUpdate(key, e.getScancode(), action, mods);
    }

    if (game && game->keyboardUpdate(key, e.getScancode(), action, mods))
        return true;

    auto& session = sceneEditor.renderAppState();
    auto& editor = sceneEditor.editorUIState();

    if (key == ToGlfwKey(caustica::Key::Space) && action == cGlfwPress
        && !ctrlDown && !altDown)
    {
        session.settings.EnableKeyframes = !session.settings.EnableKeyframes;
        return true;
    }
    if (key == ToGlfwKey(caustica::Key::F1) && action == cGlfwPress)
    {
        const bool visible = caustica::toggleNativeConsoleVisible();
        caustica::info("Native console %s", visible ? "shown" : "hidden");
        return true;
    }
    if (key == ToGlfwKey(caustica::Key::F2) && action == cGlfwPress)
        editor.ShowUI = !editor.ShowUI;
    if (key == ToGlfwKey(caustica::Key::R) && action == cGlfwPress && ctrlDown && !shiftDown && !altDown)
        session.runtime.Invalidation.ShaderReloadRequested = true;
#if CAUSTICA_WITH_STREAMLINE
    if (key == ToGlfwKey(caustica::Key::F13) && action == cGlfwPress)
        sceneEditor.app()->getGpuDevice()->getStreamline().reflexTriggerPcPing(
            sceneEditor.app()->getGpuDevice()->getFrameIndex());
#endif
    return true;
}

bool onKeyReleased(SceneEditor& sceneEditor, caustica::KeyReleasedEvent& e)
{
    const int key = ToGlfwKey(e.getKeyCode());
    const int mods = ToGlfwMods(e.getModifiers());

    imGuiForwardKeyboard(key, cGlfwRelease, e.getScancode());
    if (ImGui::GetIO().WantCaptureKeyboard)
        return true;

    auto* camera = caustica::editor::editorCamera(sceneEditor);
    if (!camera)
        return true;

    auto* zoomTool = sceneEditor.zoomTool().get();
    auto* game = sceneEditor.game().get();

    if (zoomTool && zoomTool->keyboardUpdate(key, e.getScancode(), cGlfwRelease, mods))
        return true;
    if (!(game && game->CameraActive()))
    {
        // Always accept releases so fly keys cannot stick after RMB-up.
        camera->camera().keyboardUpdate(key, e.getScancode(), cGlfwRelease, mods);
    }
    if (game && game->keyboardUpdate(key, e.getScancode(), cGlfwRelease, mods))
        return true;
    return true;
}

bool onKeyTyped(SceneEditor& sceneEditor, caustica::KeyTypedEvent& e)
{
    const unsigned int cp = e.getCodepoint();
    // ASCII `/~ : KeyPressed already toggled — only swallow so they are not typed.
    if (cp == '`' || cp == '~')
        return true;

    // Chinese · / fullwidth ｀ often arrive as char-only (no GRAVE keycode). Toggle here.
    if (cp == 0x00B7u || cp == 0xFF40u)
    {
        auto& editor = sceneEditor.editorUIState();
        editor.ShowCommandBar = !editor.ShowCommandBar;
        if (editor.ShowCommandBar)
            editor.RequestFocusCommandBar = true;
        return true;
    }

    imGuiForwardInputCharacter(cp);
    return ImGui::GetIO().WantTextInput;
}

bool onMouseMoved(SceneEditor& sceneEditor, caustica::MouseMovedEvent& e)
{
    if (g_rightClickPick.tracking)
    {
        const double dx = e.getX() - g_rightClickPick.pressX;
        const double dy = e.getY() - g_rightClickPick.pressY;
        if ((dx * dx + dy * dy) > (kRightClickPickSlopPx * kRightClickPickSlopPx))
            g_rightClickPick.dragged = true;
    }

    if (uiCapturesMouseForEditor(sceneEditor) || gizmoCapturesInput(sceneEditor))
        return false;

    auto* camera = caustica::editor::editorCamera(sceneEditor);
    if (!camera)
        return true;

    auto* game = sceneEditor.game().get();
    auto& session = sceneEditor.renderAppState();

    if (!(game && game->CameraActive()))
        camera->camera().mousePosUpdate(e.getX(), e.getY());
    if (game)
        game->mousePosUpdate(e.getX(), e.getY());

    // Display/window space — see syncPickPositionFromCursor.
    session.runtime.Picking.Position = dm::uint2{
        static_cast<uint>(e.getX()),
        static_cast<uint>(e.getY())};
    session.settings.MousePos = session.runtime.Picking.Position;

    auto* zoomTool = sceneEditor.zoomTool().get();
    if (zoomTool)
        zoomTool->mousePosUpdate(e.getX(), e.getY());
    return true;
}

bool onMouseButtonPressed(SceneEditor& sceneEditor, caustica::MouseButtonPressedEvent& e)
{
    if (uiCapturesMouseForEditor(sceneEditor) || gizmoCapturesInput(sceneEditor))
        return false;

    auto* camera = caustica::editor::editorCamera(sceneEditor);
    if (!camera)
        return true;

    const int button = ToGlfwMouse(e.getButton());
    const int mods = ToGlfwMods(e.getModifiers());

    auto* zoomTool = sceneEditor.zoomTool().get();
    auto* game = sceneEditor.game().get();
    auto& session = sceneEditor.renderAppState();

    if (zoomTool && zoomTool->mouseButtonUpdate(button, cGlfwPress, mods))
        return true;
    if (!(game && game->CameraActive()))
        camera->camera().mouseButtonUpdate(button, cGlfwPress, mods);
    if (game)
        game->mouseButtonUpdate(button, cGlfwPress, mods);
    if (button == ToGlfwMouse(caustica::Mouse::Left))
    {
        syncPickPositionFromCursor(sceneEditor);
        requestInstancePick(session.runtime);
    }
    else if (button == ToGlfwMouse(caustica::Mouse::Right))
    {
        // Defer material pick until release if the user only clicked (no fly look-drag).
        g_rightClickPick.tracking = true;
        g_rightClickPick.dragged = false;
        g_rightClickPick.pressX = 0.0;
        g_rightClickPick.pressY = 0.0;
        if (GLFWwindow* window = sceneEditor.app()->getGpuDevice()->getWindow())
            glfwGetCursorPos(window, &g_rightClickPick.pressX, &g_rightClickPick.pressY);
    }
#if CAUSTICA_WITH_STREAMLINE
    if (button == ToGlfwMouse(caustica::Mouse::Left))
        sceneEditor.app()->getGpuDevice()->getStreamline().reflexTriggerFlash(
            sceneEditor.app()->getGpuDevice()->getFrameIndex());
#endif
    return true;
}

bool onMouseButtonReleased(SceneEditor& sceneEditor, caustica::MouseButtonReleasedEvent& e)
{
    const int button = ToGlfwMouse(e.getButton());
    const int mods = ToGlfwMods(e.getModifiers());

    if (button == ToGlfwMouse(caustica::Mouse::Right) && g_rightClickPick.tracking)
    {
        const bool clicked = !g_rightClickPick.dragged;
        g_rightClickPick = {};
        if (clicked && !uiCapturesMouseForEditor(sceneEditor) && !gizmoCapturesInput(sceneEditor))
        {
            syncPickPositionFromCursor(sceneEditor);
            requestMaterialPick(sceneEditor.renderAppState().runtime);
        }
    }

    if (uiCapturesMouseForEditor(sceneEditor) || gizmoCapturesInput(sceneEditor))
        return false;

    auto* camera = caustica::editor::editorCamera(sceneEditor);
    if (!camera)
        return true;

    auto* zoomTool = sceneEditor.zoomTool().get();
    auto* game = sceneEditor.game().get();

    if (zoomTool && zoomTool->mouseButtonUpdate(button, cGlfwRelease, mods))
        return true;
    if (!(game && game->CameraActive()))
    {
        camera->camera().mouseButtonUpdate(button, cGlfwRelease, mods);
        if (button == ToGlfwMouse(caustica::Mouse::Right))
            camera->camera().clearFlyKeyboardState();
    }
    if (game)
        game->mouseButtonUpdate(button, cGlfwRelease, mods);
    return true;
}

bool onMouseScrolled(SceneEditor& sceneEditor, caustica::MouseScrolledEvent& e)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseWheelEvent(static_cast<float>(e.getXOffset()), static_cast<float>(e.getYOffset()));
    if (uiCapturesMouseForEditor(sceneEditor) || gizmoCapturesInput(sceneEditor))
        return true;

    auto* camera = caustica::editor::editorCamera(sceneEditor);
    auto* game = sceneEditor.game().get();
    if (!camera || (game && game->CameraActive()))
        return true;

    // Alt + wheel: change fly speed. Plain wheel: dolly zoom in/out.
    const bool altHeld = io.KeyAlt;
    if (altHeld)
    {
        float& speed = sceneEditor.renderAppState().settings.CameraMoveSpeed;
        speed = std::clamp(speed * (1.0f + static_cast<float>(e.getYOffset()) * 0.1f), 0.01f, 100.f);
    }
    else
    {
        camera->camera().mouseScrollUpdate(e.getXOffset(), e.getYOffset());
    }
    return true;
}

} // namespace

void EditorInputRouter::bind(SceneEditor& sceneEditor)
{
    m_sceneEditor = &sceneEditor;
}

void EditorInputRouter::onEvent(caustica::Event& event)
{
    if (!m_sceneEditor)
        return;

    SceneEditor& sceneEditor = *m_sceneEditor;
    caustica::EventDispatcher dispatcher(event);
    dispatcher.dispatch<caustica::KeyPressedEvent>([&](auto& e) { return onKeyPressed(sceneEditor, e); });
    dispatcher.dispatch<caustica::KeyReleasedEvent>([&](auto& e) { return onKeyReleased(sceneEditor, e); });
    dispatcher.dispatch<caustica::KeyTypedEvent>([&](auto& e) { return onKeyTyped(sceneEditor, e); });
    dispatcher.dispatch<caustica::MouseMovedEvent>([&](auto& e) { return onMouseMoved(sceneEditor, e); });
    dispatcher.dispatch<caustica::MouseButtonPressedEvent>([&](auto& e) { return onMouseButtonPressed(sceneEditor, e); });
    dispatcher.dispatch<caustica::MouseButtonReleasedEvent>([&](auto& e) { return onMouseButtonReleased(sceneEditor, e); });
    dispatcher.dispatch<caustica::MouseScrolledEvent>([&](auto& e) { return onMouseScrolled(sceneEditor, e); });
}

} // namespace caustica::editor

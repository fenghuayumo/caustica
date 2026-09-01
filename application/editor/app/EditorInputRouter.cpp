#include <algorithm>
#include <cmath>
#include "EditorInputRouter.h"

#include "SceneEditor.h"
#include "EditorAccess.h"
#include "EditorUIState.h"
#include <engine/RenderSessionApi.h>
#include <engine/CameraApi.h>

#include <backend/GpuDevice.h>
#include <GLFW/glfw3.h>
#include <core/log.h>
#include <ecs/Entity.h>
#include <events/event.h>
#include <events/key_event.h>
#include <events/mouse_event.h>
#include <imgui.h>
#include <ImGuizmo.h>
#include <imgui/imgui_renderer.h>
#include <render/RenderAppState.h>
#include "game/GameScene.h"
#include <render/passes/debug/ZoomTool.h>

namespace caustica::editor
{

namespace
{
bool mouseInViewportCanvas(const SceneEditor& sceneEditor, double cursorX, double cursorY)
{
    const auto& vp = sceneEditor.editorUIState().Viewport;
    if (!vp.RectValid || vp.SizeX <= 1.f || vp.SizeY <= 1.f)
        return false;
    return cursorX >= static_cast<double>(vp.PosX)
        && cursorY >= static_cast<double>(vp.PosY)
        && cursorX < static_cast<double>(vp.PosX + vp.SizeX)
        && cursorY < static_cast<double>(vp.PosY + vp.SizeY);
}

bool uiCapturesMouseForEditor(const SceneEditor& sceneEditor)
{
    const auto& vp = sceneEditor.editorUIState().Viewport;
    GLFWwindow* window = sceneEditor.app() && sceneEditor.app()->getGpuDevice()
        ? sceneEditor.app()->getGpuDevice()->getWindow()
        : nullptr;
    double cursorX = 0.0;
    double cursorY = 0.0;
    if (window)
        glfwGetCursorPos(window, &cursorX, &cursorY);

    // Hit-test the canvas rect at event time. Viewport.Hovered is from last UI
    // frame and is often false: no InvisibleButton, and ImGuizmo BeginFrame's
    // NoInputs window can become HoveredWindow.
    if (mouseInViewportCanvas(sceneEditor, cursorX, cursorY) && !vp.OverlayHovered)
        return false;

    if (!ImGui::GetIO().WantCaptureMouse)
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

bool altHeld(int mods)
{
    return (mods & GLFW_MOD_ALT) != 0;
}

bool cursorInViewportForCamera(const SceneEditor& sceneEditor)
{
    const auto& vp = sceneEditor.editorUIState().Viewport;
    if (vp.OverlayHovered)
        return false;

    GLFWwindow* window = sceneEditor.app() && sceneEditor.app()->getGpuDevice()
        ? sceneEditor.app()->getGpuDevice()->getWindow()
        : nullptr;
    double cursorX = 0.0;
    double cursorY = 0.0;
    if (window)
        glfwGetCursorPos(window, &cursorX, &cursorY);
    return mouseInViewportCanvas(sceneEditor, cursorX, cursorY);
}

bool gizmoCapturesInput(const SceneEditor& sceneEditor)
{
    // Alt+LMB is camera look; gizmos must not swallow that chord.
    if (ImGui::GetIO().KeyAlt)
        return false;

    const auto& editor = sceneEditor.editorUIState();
    if (editor.GizmoCapturingInput)
        return true;
    // IsOver() is only meaningful after Manipulate() this/last frame. Calling it
    // with no selected gizmo (or after DrawGrid's leftover context) blocks picks.
    if (!editor.GizmoEnabled || !caustica::ecs::isValid(editor.SelectedEntity))
        return false;
    return ImGuizmo::IsOver() || ImGuizmo::IsUsing();
}

void activateFreeCameraForInput(App& app)
{
    if (caustica::selectedCameraIndex(app) == 0)
        return;

    // Scene cameras are authoritative and are refreshed from ECS every frame.
    // Switch to the already-synced controller pose before applying viewport
    // input, so the first interaction preserves the reference view and then
    // remains freely editable.
    caustica::selectedCameraIndex(app) = 0;
    caustica::markCameraChanged(app);
}

void requestMaterialPick(SceneEditor& sceneEditor)
{
    // Pick the hit geometry's material (per sub-mesh), not the whole instance.
    auto& picking = sceneEditor.renderAppState().runtime.Picking;
    picking.requestMaterialPick();
    caustica::submitImmediateMaterialPick(editorApp(sceneEditor), picking);
}

void requestInstancePick(SceneEditor& sceneEditor)
{
    // Left-click: select the mesh instance entity for Inspector / gizmo.
    auto& picking = sceneEditor.renderAppState().runtime.Picking;
    picking.requestInstancePick();
    caustica::submitImmediateInstancePick(editorApp(sceneEditor), picking);
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

    App* app = sceneEditor.app();
    if (!app)
        return true;

    auto* zoomTool = sceneEditor.zoomTool().get();
    auto* game = sceneEditor.game().get();

    if (zoomTool && zoomTool->keyboardUpdate(key, e.getScancode(), action, mods))
        return true;

    // WASD/QE fly only while RMB is held so Q/T/R/S gizmo hotkeys stay free.
    if (!(game && game->CameraActive()))
    {
        if (!isCameraFlyKey(key) || isRightMouseDown(sceneEditor))
        {
            activateFreeCameraForInput(*app);
            caustica::currentCamera(*app).keyboardUpdate(key, e.getScancode(), action, mods);
        }
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

    App* app = sceneEditor.app();
    if (!app)
        return true;

    auto* zoomTool = sceneEditor.zoomTool().get();
    auto* game = sceneEditor.game().get();

    if (zoomTool && zoomTool->keyboardUpdate(key, e.getScancode(), cGlfwRelease, mods))
        return true;
    if (!(game && game->CameraActive()))
    {
        // Always accept releases so fly keys cannot stick after RMB-up.
        caustica::currentCamera(*app).keyboardUpdate(key, e.getScancode(), cGlfwRelease, mods);
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
    App* app = sceneEditor.app();
    auto* game = sceneEditor.game().get();

    // Always track cursor for look/pan deltas. Those modes only apply while
    // their buttons are held, so this is safe over gizmos and side panels.
    if (app && !(game && game->CameraActive()))
        caustica::currentCamera(*app).mousePosUpdate(e.getX(), e.getY());
    if (game)
        game->mousePosUpdate(e.getX(), e.getY());

    auto* zoomTool = sceneEditor.zoomTool().get();
    if (zoomTool)
        zoomTool->mousePosUpdate(e.getX(), e.getY());

    if (uiCapturesMouseForEditor(sceneEditor) || gizmoCapturesInput(sceneEditor))
        return false;

    auto& session = sceneEditor.renderAppState();
    syncPickPositionFromCursor(sceneEditor);
    session.settings.MousePos = session.runtime.Picking.Position;
    return true;
}

bool onMouseButtonPressed(SceneEditor& sceneEditor, caustica::MouseButtonPressedEvent& e)
{
    App* app = sceneEditor.app();
    if (!app)
        return true;

    const int button = ToGlfwMouse(e.getButton());
    const int mods = ToGlfwMods(e.getModifiers());
    const bool isLeft = button == ToGlfwMouse(caustica::Mouse::Left);
    const bool isMiddle = button == ToGlfwMouse(caustica::Mouse::Middle);

    auto* zoomTool = sceneEditor.zoomTool().get();
    auto* game = sceneEditor.game().get();
    auto& session = sceneEditor.renderAppState();
    auto& editor = sceneEditor.editorUIState();

    // Alt+LMB looks, including over transform gizmos, so grabbing a rotate
    // axis cannot also tumble the camera. Plain LMB is pick / gizmo only.
    if (isLeft && altHeld(mods) && cursorInViewportForCamera(sceneEditor)
        && !(game && game->CameraActive()))
    {
        activateFreeCameraForInput(*app);
        caustica::currentCamera(*app).mouseButtonUpdate(button, cGlfwPress, mods);
        return true;
    }

    if (uiCapturesMouseForEditor(sceneEditor) || gizmoCapturesInput(sceneEditor))
        return false;

    if (zoomTool && zoomTool->mouseButtonUpdate(button, cGlfwPress, mods))
        return true;

    // While the eyedropper is active, each LMB click samples a material. Keep the
    // mode armed so the user can inspect several objects without returning to the toolbar.
    if (isLeft && editor.MaterialPickerActive)
    {
        syncPickPositionFromCursor(sceneEditor);
        requestMaterialPick(sceneEditor);
        return true;
    }

    // MMB pans. RMB is fly (WASD), not look. LMB look requires Alt, above.
    if (!(game && game->CameraActive()))
    {
        if (isLeft || isMiddle)
            activateFreeCameraForInput(*app);
        if (isMiddle)
            caustica::currentCamera(*app).mouseButtonUpdate(button, cGlfwPress, mods);
    }
    if (game)
        game->mouseButtonUpdate(button, cGlfwPress, mods);
    if (isLeft)
    {
        syncPickPositionFromCursor(sceneEditor);
        requestInstancePick(sceneEditor);
    }
#if CAUSTICA_WITH_STREAMLINE
    if (isLeft)
        sceneEditor.app()->getGpuDevice()->getStreamline().reflexTriggerFlash(
            sceneEditor.app()->getGpuDevice()->getFrameIndex());
#endif
    return true;
}

bool onMouseButtonReleased(SceneEditor& sceneEditor, caustica::MouseButtonReleasedEvent& e)
{
    const int button = ToGlfwMouse(e.getButton());
    const int mods = ToGlfwMods(e.getModifiers());

    App* app = sceneEditor.app();
    auto* game = sceneEditor.game().get();

    // Always clear look/pan so Alt+LMB cannot stick if release happens over a gizmo.
    if (app && !(game && game->CameraActive()))
    {
        if (button != ToGlfwMouse(caustica::Mouse::Right))
            caustica::currentCamera(*app).mouseButtonUpdate(button, cGlfwRelease, mods);
        if (button == ToGlfwMouse(caustica::Mouse::Right))
            caustica::currentCamera(*app).clearFlyKeyboardState();
    }

    if (uiCapturesMouseForEditor(sceneEditor) || gizmoCapturesInput(sceneEditor))
        return false;

    if (!app)
        return true;

    auto* zoomTool = sceneEditor.zoomTool().get();

    if (zoomTool && zoomTool->mouseButtonUpdate(button, cGlfwRelease, mods))
        return true;
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

    App* app = sceneEditor.app();
    auto* game = sceneEditor.game().get();
    if (!app || (game && game->CameraActive()))
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
        activateFreeCameraForInput(*app);
        caustica::currentCamera(*app).mouseScrollUpdate(e.getXOffset(), e.getYOffset());
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

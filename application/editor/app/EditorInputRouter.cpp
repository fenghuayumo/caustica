#include "EditorInputRouter.h"

#include "SceneEditor.h"
#include "EditorAccess.h"
#include "EditorUIState.h"

#include <backend/GpuDevice.h>
#include <core/log.h>
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
inline constexpr int ToGlfwKey(caustica::KeyCode k) { return static_cast<int>(k); }
inline constexpr int ToGlfwMouse(caustica::MouseCode m) { return static_cast<int>(m); }
inline constexpr int ToGlfwMods(caustica::ModifierKey m) { return static_cast<int>(m); }
inline constexpr int cGlfwPress = 1;
inline constexpr int cGlfwRelease = 0;
inline constexpr int cGlfwRepeat = 2;

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

    // Keep display/window space here. Path-trace pick pixels are derived later
    // from this frame's settled renderSize (after DLSS), not from a live
    // WorldRenderer::getRenderSize() that the render thread mutates mid-frame.
    session.runtime.Picking.Position = dm::uint2{
        static_cast<uint>(cursorX),
        static_cast<uint>(cursorY)};
    session.settings.MousePos = session.runtime.Picking.Position;
    session.settings.DebugPixel = session.runtime.Picking.Position;
}

bool onKeyPressed(SceneEditor& sceneEditor, caustica::KeyPressedEvent& e)
{
    const int key = ToGlfwKey(e.getKeyCode());
    const int mods = ToGlfwMods(e.getModifiers());
    const int action = e.isRepeat() ? cGlfwRepeat : cGlfwPress;

    imGuiForwardKeyboard(key, action, e.getScancode());
    if (ImGui::GetIO().WantCaptureKeyboard)
        return true;

    auto* camera = caustica::editor::editorCamera(sceneEditor);
    if (!camera)
        return true;

    auto* zoomTool = sceneEditor.zoomTool().get();
    auto* game = sceneEditor.game().get();

    if (zoomTool && zoomTool->keyboardUpdate(key, e.getScancode(), action, mods))
        return true;

    if (!(game && game->CameraActive()))
        camera->camera().keyboardUpdate(key, e.getScancode(), action, mods);

    if (game && game->keyboardUpdate(key, e.getScancode(), action, mods))
        return true;

    auto& session = sceneEditor.renderAppState();
    auto& editor = sceneEditor.editorUIState();

    if (key == ToGlfwKey(caustica::Key::Space) && action == cGlfwPress
        && mods != ToGlfwMods(caustica::ModifierKey::Control)
        && mods != ToGlfwMods(caustica::ModifierKey::Alt))
    {
        session.settings.EnableAnimations = !session.settings.EnableAnimations;
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
    if (key == ToGlfwKey(caustica::Key::R) && action == cGlfwPress
        && mods == ToGlfwMods(caustica::ModifierKey::Control))
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
        camera->camera().keyboardUpdate(key, e.getScancode(), cGlfwRelease, mods);
    if (game && game->keyboardUpdate(key, e.getScancode(), cGlfwRelease, mods))
        return true;
    return true;
}

bool onKeyTyped(SceneEditor& /*sceneEditor*/, caustica::KeyTypedEvent& e)
{
    imGuiForwardInputCharacter(e.getCodepoint());
    return ImGui::GetIO().WantTextInput;
}

bool onMouseMoved(SceneEditor& sceneEditor, caustica::MouseMovedEvent& e)
{
    if (ImGui::GetIO().WantCaptureMouse || gizmoCapturesInput(sceneEditor))
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
    if (ImGui::GetIO().WantCaptureMouse || gizmoCapturesInput(sceneEditor))
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
        syncPickPositionFromCursor(sceneEditor);
        requestMaterialPick(session.runtime);
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
    if (ImGui::GetIO().WantCaptureMouse || gizmoCapturesInput(sceneEditor))
        return false;

    auto* camera = caustica::editor::editorCamera(sceneEditor);
    if (!camera)
        return true;

    const int button = ToGlfwMouse(e.getButton());
    const int mods = ToGlfwMods(e.getModifiers());

    auto* zoomTool = sceneEditor.zoomTool().get();
    auto* game = sceneEditor.game().get();

    if (zoomTool && zoomTool->mouseButtonUpdate(button, cGlfwRelease, mods))
        return true;
    if (!(game && game->CameraActive()))
        camera->camera().mouseButtonUpdate(button, cGlfwRelease, mods);
    if (game)
        game->mouseButtonUpdate(button, cGlfwRelease, mods);
    return true;
}

bool onMouseScrolled(SceneEditor& sceneEditor, caustica::MouseScrolledEvent& e)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseWheelEvent(static_cast<float>(e.getXOffset()), static_cast<float>(e.getYOffset()));
    if (io.WantCaptureMouse)
        return true;

    auto* game = sceneEditor.game().get();
    if (!(game && game->CameraActive()))
        sceneEditor.renderAppState().settings.CameraMoveSpeed *= 1.0f + static_cast<float>(e.getYOffset()) * 0.1f;
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

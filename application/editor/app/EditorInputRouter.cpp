#include "EditorInputRouter.h"

#include "SceneEditor.h"
#include "EditorUIState.h"

#include <backend/GpuDevice.h>
#include <events/event.h>
#include <events/key_event.h>
#include <events/mouse_event.h>
#include <imgui.h>
#include <imgui/imgui_renderer.h>
#include <render/RenderSessionState.h>
#include <render/worldRenderer/WorldRenderer.h>
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

bool onKeyPressed(SceneEditor& sceneEditor, caustica::KeyPressedEvent& e)
{
    const int key = ToGlfwKey(e.GetKeyCode());
    const int mods = ToGlfwMods(e.GetModifiers());
    const int action = e.IsRepeat() ? cGlfwRepeat : cGlfwPress;

    ImGuiForwardKeyboard(key, action, e.GetScancode());
    if (ImGui::GetIO().WantCaptureKeyboard)
        return true;

    auto* gpuRender = sceneEditor.gpuRender();
    if (!gpuRender)
        return true;

    auto* zoomTool = sceneEditor.GetZoomTool().get();
    auto* game = sceneEditor.GetGame().get();
    auto* camera = &gpuRender->camera();

    if (zoomTool && zoomTool->KeyboardUpdate(key, e.GetScancode(), action, mods))
        return true;

    if (!(game && game->CameraActive()))
        camera->camera().KeyboardUpdate(key, e.GetScancode(), action, mods);

    if (game && game->KeyboardUpdate(key, e.GetScancode(), action, mods))
        return true;

    auto& session = sceneEditor.renderSessionState();
    auto& editor = sceneEditor.GetEditorUIState();

    if (key == ToGlfwKey(caustica::Key::Space) && action == cGlfwPress
        && mods != ToGlfwMods(caustica::ModifierKey::Control)
        && mods != ToGlfwMods(caustica::ModifierKey::Alt))
    {
        session.settings.EnableAnimations = !session.settings.EnableAnimations;
        return true;
    }
    if (key == ToGlfwKey(caustica::Key::F2) && action == cGlfwPress)
        editor.ShowUI = !editor.ShowUI;
    if (key == ToGlfwKey(caustica::Key::R) && action == cGlfwPress
        && mods == ToGlfwMods(caustica::ModifierKey::Control))
        session.runtime.Invalidation.ShaderReloadRequested = true;
#if CAUSTICA_WITH_STREAMLINE
    if (key == ToGlfwKey(caustica::Key::F13) && action == cGlfwPress)
        sceneEditor.gpuDevice().GetStreamline().ReflexTriggerPcPing(sceneEditor.frameIndex());
#endif
    return true;
}

bool onKeyReleased(SceneEditor& sceneEditor, caustica::KeyReleasedEvent& e)
{
    const int key = ToGlfwKey(e.GetKeyCode());
    const int mods = ToGlfwMods(e.GetModifiers());

    ImGuiForwardKeyboard(key, cGlfwRelease, e.GetScancode());
    if (ImGui::GetIO().WantCaptureKeyboard)
        return true;

    auto* gpuRender = sceneEditor.gpuRender();
    if (!gpuRender)
        return true;

    auto* zoomTool = sceneEditor.GetZoomTool().get();
    auto* game = sceneEditor.GetGame().get();
    auto* camera = &gpuRender->camera();

    if (zoomTool && zoomTool->KeyboardUpdate(key, e.GetScancode(), cGlfwRelease, mods))
        return true;
    if (!(game && game->CameraActive()))
        camera->camera().KeyboardUpdate(key, e.GetScancode(), cGlfwRelease, mods);
    if (game && game->KeyboardUpdate(key, e.GetScancode(), cGlfwRelease, mods))
        return true;
    return true;
}

bool onKeyTyped(SceneEditor& /*sceneEditor*/, caustica::KeyTypedEvent& e)
{
    ImGuiForwardInputCharacter(e.GetCodepoint());
    return ImGui::GetIO().WantTextInput;
}

bool onMouseMoved(SceneEditor& sceneEditor, caustica::MouseMovedEvent& e)
{
    if (ImGui::GetIO().WantCaptureMouse)
        return false;

    auto* gpuRender = sceneEditor.gpuRender();
    if (!gpuRender)
        return true;

    auto* game = sceneEditor.GetGame().get();
    auto* camera = &gpuRender->camera();
    auto* worldRenderer = sceneEditor.worldRenderer();
    auto& session = sceneEditor.renderSessionState();

    if (!(game && game->CameraActive()))
        camera->camera().MousePosUpdate(e.GetX(), e.GetY());
    if (game)
        game->MousePosUpdate(e.GetX(), e.GetY());

    dm::float2 upscalingScale(1.0f, 1.0f);
    if (worldRenderer && worldRenderer->getRenderTargets())
        upscalingScale = dm::float2(worldRenderer->getRenderSize())
            / dm::float2(worldRenderer->getDisplaySize());

    session.runtime.Picking.Position = dm::uint2{
        static_cast<uint>(e.GetX() * upscalingScale.x),
        static_cast<uint>(e.GetY() * upscalingScale.y)};
    session.settings.MousePos = session.runtime.Picking.Position;

    auto* zoomTool = sceneEditor.GetZoomTool().get();
    if (zoomTool)
        zoomTool->MousePosUpdate(e.GetX(), e.GetY());
    return true;
}

bool onMouseButtonPressed(SceneEditor& sceneEditor, caustica::MouseButtonPressedEvent& e)
{
    if (ImGui::GetIO().WantCaptureMouse)
        return false;

    auto* gpuRender = sceneEditor.gpuRender();
    if (!gpuRender)
        return true;

    const int button = ToGlfwMouse(e.GetButton());
    const int mods = ToGlfwMods(e.GetModifiers());

    auto* zoomTool = sceneEditor.GetZoomTool().get();
    auto* game = sceneEditor.GetGame().get();
    auto* camera = &gpuRender->camera();
    auto& session = sceneEditor.renderSessionState();

    if (zoomTool && zoomTool->MouseButtonUpdate(button, cGlfwPress, mods))
        return true;
    if (!(game && game->CameraActive()))
        camera->camera().MouseButtonUpdate(button, cGlfwPress, mods);
    if (game)
        game->MouseButtonUpdate(button, cGlfwPress, mods);
    if (button == ToGlfwMouse(caustica::Mouse::Right))
    {
        session.runtime.Picking.MaterialRequested = true;
        session.runtime.Picking.InstanceRequested = true;
        session.settings.DebugPixel = session.runtime.Picking.Position;
    }
#if CAUSTICA_WITH_STREAMLINE
    if (button == ToGlfwMouse(caustica::Mouse::Left))
        sceneEditor.gpuDevice().GetStreamline().ReflexTriggerFlash(sceneEditor.frameIndex());
#endif
    return true;
}

bool onMouseButtonReleased(SceneEditor& sceneEditor, caustica::MouseButtonReleasedEvent& e)
{
    if (ImGui::GetIO().WantCaptureMouse)
        return false;

    auto* gpuRender = sceneEditor.gpuRender();
    if (!gpuRender)
        return true;

    const int button = ToGlfwMouse(e.GetButton());
    const int mods = ToGlfwMods(e.GetModifiers());

    auto* zoomTool = sceneEditor.GetZoomTool().get();
    auto* game = sceneEditor.GetGame().get();
    auto* camera = &gpuRender->camera();

    if (zoomTool && zoomTool->MouseButtonUpdate(button, cGlfwRelease, mods))
        return true;
    if (!(game && game->CameraActive()))
        camera->camera().MouseButtonUpdate(button, cGlfwRelease, mods);
    if (game)
        game->MouseButtonUpdate(button, cGlfwRelease, mods);
    return true;
}

bool onMouseScrolled(SceneEditor& sceneEditor, caustica::MouseScrolledEvent& e)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseWheelEvent(static_cast<float>(e.GetXOffset()), static_cast<float>(e.GetYOffset()));
    if (io.WantCaptureMouse)
        return true;

    auto* game = sceneEditor.GetGame().get();
    if (!(game && game->CameraActive()))
        sceneEditor.renderSessionState().settings.CameraMoveSpeed *= 1.0f + static_cast<float>(e.GetYOffset()) * 0.1f;
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
    dispatcher.Dispatch<caustica::KeyPressedEvent>([&](auto& e) { return onKeyPressed(sceneEditor, e); });
    dispatcher.Dispatch<caustica::KeyReleasedEvent>([&](auto& e) { return onKeyReleased(sceneEditor, e); });
    dispatcher.Dispatch<caustica::KeyTypedEvent>([&](auto& e) { return onKeyTyped(sceneEditor, e); });
    dispatcher.Dispatch<caustica::MouseMovedEvent>([&](auto& e) { return onMouseMoved(sceneEditor, e); });
    dispatcher.Dispatch<caustica::MouseButtonPressedEvent>([&](auto& e) { return onMouseButtonPressed(sceneEditor, e); });
    dispatcher.Dispatch<caustica::MouseButtonReleasedEvent>([&](auto& e) { return onMouseButtonReleased(sceneEditor, e); });
    dispatcher.Dispatch<caustica::MouseScrolledEvent>([&](auto& e) { return onMouseScrolled(sceneEditor, e); });
}

} // namespace caustica::editor

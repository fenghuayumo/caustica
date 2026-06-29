#include "EditorInputRouter.h"

#include "EditorUIState.h"

#include <backend/GpuDevice.h>
#include <events/event.h>
#include <events/key_event.h>
#include <events/mouse_event.h>
#include <imgui.h>
#include <render/RenderSessionState.h>
#include <render/WorldRenderer/PathTracingWorldRenderer.h>
#include "game/GameScene.h"
#include <render/Passes/Debug/ZoomTool.h>

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

bool onKeyPressed(EditorInputRouter::Context& ctx, caustica::KeyPressedEvent& e)
{
    int key = ToGlfwKey(e.GetKeyCode());
    int mods = ToGlfwMods(e.GetModifiers());
    int action = e.IsRepeat() ? cGlfwRepeat : cGlfwPress;

    if (ctx.zoomTool && ctx.zoomTool->KeyboardUpdate(key, e.GetScancode(), action, mods))
        return true;

    if (!(ctx.game && ctx.game->CameraActive()))
        ctx.renderCore->camera().camera().KeyboardUpdate(key, e.GetScancode(), action, mods);

    if (ctx.game && ctx.game->KeyboardUpdate(key, e.GetScancode(), action, mods))
        return true;

    if (key == ToGlfwKey(caustica::Key::Space) && action == cGlfwPress
        && mods != ToGlfwMods(caustica::ModifierKey::Control)
        && mods != ToGlfwMods(caustica::ModifierKey::Alt))
    {
        ctx.session.EnableAnimations = !ctx.session.EnableAnimations;
        return true;
    }
    if (key == ToGlfwKey(caustica::Key::F2) && action == cGlfwPress)
        ctx.editor.ShowUI = !ctx.editor.ShowUI;
    if (key == ToGlfwKey(caustica::Key::R) && action == cGlfwPress
        && mods == ToGlfwMods(caustica::ModifierKey::Control))
        ctx.session.Invalidation.ShaderReloadRequested = true;
#if CAUSTICA_WITH_STREAMLINE
    if (key == ToGlfwKey(caustica::Key::F13) && action == cGlfwPress)
        ctx.gpuDevice->GetStreamline().ReflexTriggerPcPing(ctx.gpuDevice->GetFrameIndex());
#endif
    return true;
}

bool onKeyReleased(EditorInputRouter::Context& ctx, caustica::KeyReleasedEvent& e)
{
    int key = ToGlfwKey(e.GetKeyCode());
    int mods = ToGlfwMods(e.GetModifiers());
    if (ctx.zoomTool && ctx.zoomTool->KeyboardUpdate(key, e.GetScancode(), cGlfwRelease, mods))
        return true;
    if (!(ctx.game && ctx.game->CameraActive()))
        ctx.renderCore->camera().camera().KeyboardUpdate(key, e.GetScancode(), cGlfwRelease, mods);
    if (ctx.game && ctx.game->KeyboardUpdate(key, e.GetScancode(), cGlfwRelease, mods))
        return true;
    return true;
}

bool onMouseMoved(EditorInputRouter::Context& ctx, caustica::MouseMovedEvent& e)
{
    if (ImGui::GetIO().WantCaptureMouse) return false;
    if (!(ctx.game && ctx.game->CameraActive()))
        ctx.renderCore->camera().camera().MousePosUpdate(e.GetX(), e.GetY());
    if (ctx.game)
        ctx.game->MousePosUpdate(e.GetX(), e.GetY());

    dm::float2 upscalingScale(1.0f, 1.0f);
    if (ctx.worldRenderer && ctx.worldRenderer->getRenderTargets())
        upscalingScale = dm::float2(ctx.worldRenderer->getRenderSize())
            / dm::float2(ctx.worldRenderer->getDisplaySize());

    ctx.session.Picking.Position = dm::uint2{
        static_cast<uint>(e.GetX() * upscalingScale.x),
        static_cast<uint>(e.GetY() * upscalingScale.y)};
    ctx.session.MousePos = ctx.session.Picking.Position;

    if (ctx.zoomTool) ctx.zoomTool->MousePosUpdate(e.GetX(), e.GetY());
    return true;
}

bool onMouseButtonPressed(EditorInputRouter::Context& ctx, caustica::MouseButtonPressedEvent& e)
{
    if (ImGui::GetIO().WantCaptureMouse) return false;
    int button = ToGlfwMouse(e.GetButton());
    int mods = ToGlfwMods(e.GetModifiers());
    if (ctx.zoomTool && ctx.zoomTool->MouseButtonUpdate(button, cGlfwPress, mods)) return true;
    if (!(ctx.game && ctx.game->CameraActive()))
        ctx.renderCore->camera().camera().MouseButtonUpdate(button, cGlfwPress, mods);
    if (ctx.game)
        ctx.game->MouseButtonUpdate(button, cGlfwPress, mods);
    if (button == ToGlfwMouse(caustica::Mouse::Right))
    {
        ctx.session.Picking.MaterialRequested = true;
        ctx.session.Picking.InstanceRequested = true;
        ctx.session.DebugPixel = ctx.session.Picking.Position;
    }
#if CAUSTICA_WITH_STREAMLINE
    if (button == ToGlfwMouse(caustica::Mouse::Left))
        ctx.gpuDevice->GetStreamline().ReflexTriggerFlash(ctx.gpuDevice->GetFrameIndex());
#endif
    return true;
}

bool onMouseButtonReleased(EditorInputRouter::Context& ctx, caustica::MouseButtonReleasedEvent& e)
{
    if (ImGui::GetIO().WantCaptureMouse) return false;
    int button = ToGlfwMouse(e.GetButton());
    int mods = ToGlfwMods(e.GetModifiers());
    if (ctx.zoomTool && ctx.zoomTool->MouseButtonUpdate(button, cGlfwRelease, mods)) return true;
    if (!(ctx.game && ctx.game->CameraActive()))
        ctx.renderCore->camera().camera().MouseButtonUpdate(button, cGlfwRelease, mods);
    if (ctx.game)
        ctx.game->MouseButtonUpdate(button, cGlfwRelease, mods);
    return true;
}

bool onMouseScrolled(EditorInputRouter::Context& ctx, caustica::MouseScrolledEvent& e)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseWheelEvent(static_cast<float>(e.GetXOffset()), static_cast<float>(e.GetYOffset()));
    if (io.WantCaptureMouse) return true;
    if (!(ctx.game && ctx.game->CameraActive()))
        ctx.session.CameraMoveSpeed *= 1.0f + static_cast<float>(e.GetYOffset()) * 0.1f;
    return true;
}

} // namespace

EditorInputRouter::EditorInputRouter(Context context)
    : m_ctx(context)
{
}

void EditorInputRouter::onEvent(caustica::Event& event)
{
    caustica::EventDispatcher dispatcher(event);
    dispatcher.Dispatch<caustica::KeyPressedEvent>([this](auto& e) { return onKeyPressed(m_ctx, e); });
    dispatcher.Dispatch<caustica::KeyReleasedEvent>([this](auto& e) { return onKeyReleased(m_ctx, e); });
    dispatcher.Dispatch<caustica::MouseMovedEvent>([this](auto& e) { return onMouseMoved(m_ctx, e); });
    dispatcher.Dispatch<caustica::MouseButtonPressedEvent>([this](auto& e) { return onMouseButtonPressed(m_ctx, e); });
    dispatcher.Dispatch<caustica::MouseButtonReleasedEvent>([this](auto& e) { return onMouseButtonReleased(m_ctx, e); });
    dispatcher.Dispatch<caustica::MouseScrolledEvent>([this](auto& e) { return onMouseScrolled(m_ctx, e); });
}

} // namespace caustica::editor

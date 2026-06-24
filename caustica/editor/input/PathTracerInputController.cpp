#include "input/PathTracerInputController.h"

#include "EditorUIState.h"
#include <render/Core/PathTracerSettings.h>
#include <render/Core/RenderCore.h>
#include <render/Passes/Debug/ZoomTool.h>
#include <backend/GpuDevice.h>
#include <scene/camera/Camera.h>

#include "SampleGame/GameScene.h"

#include <imgui.h>
#include <GLFW/glfw3.h>

#if CAUSTICA_WITH_STREAMLINE
#include <engine/StreamlineInterface.h>
#endif

PathTracerInputController::PathTracerInputController(Bindings bindings)
    : m_bindings(std::move(bindings))
{
}

bool PathTracerInputController::onKeyEvent(int key, int scancode, int action, int mods)
{
    ZoomTool* zoomTool = m_bindings.getZoomTool ? m_bindings.getZoomTool() : nullptr;
    if (zoomTool && zoomTool->KeyboardUpdate(key, scancode, action, mods))
        return true;

    GameScene* game = m_bindings.sampleGame;
    if (!(game && game->CameraActive()))
        m_bindings.renderCore.camera().camera().KeyboardUpdate(key, scancode, action, mods);

    if (game && game->KeyboardUpdate(key, scancode, action, mods))
        return true;

    if (key == GLFW_KEY_SPACE && action == GLFW_PRESS && mods != GLFW_MOD_CONTROL && mods != GLFW_MOD_ALT)
    {
        m_bindings.settings.EnableAnimations = !m_bindings.settings.EnableAnimations;
        return true;
    }
    if (key == GLFW_KEY_F2 && action == GLFW_PRESS)
        m_bindings.editor.ShowUI = !m_bindings.editor.ShowUI;
    if (key == GLFW_KEY_R && action == GLFW_PRESS && mods == GLFW_MOD_CONTROL)
        m_bindings.editor.ShaderReloadRequested = true;

#if CAUSTICA_WITH_STREAMLINE
    if (key == GLFW_KEY_F13 && action == GLFW_PRESS)
        m_bindings.gpuDevice.GetStreamline().ReflexTriggerPcPing(m_bindings.gpuDevice.GetFrameIndex());
#endif

    return true;
}

bool PathTracerInputController::onMouseMoveEvent(double xpos, double ypos)
{
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

bool PathTracerInputController::onMouseButtonEvent(int button, int action, int mods)
{
    if (ImGui::GetIO().WantCaptureMouse)
        return false;

    if (ZoomTool* zoomTool = m_bindings.getZoomTool ? m_bindings.getZoomTool() : nullptr)
        if (zoomTool->MouseButtonUpdate(button, action, mods))
            return true;

    GameScene* game = m_bindings.sampleGame;
    if (!(game && game->CameraActive()))
        m_bindings.renderCore.camera().camera().MouseButtonUpdate(button, action, mods);
    if (game)
        game->MouseButtonUpdate(button, action, mods);

    if (action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_2)
    {
        m_bindings.editor.PickMaterialRequested = true;
        m_bindings.editor.PickInstanceRequested = true;
        m_bindings.settings.DebugPixel = m_bindings.editor.PickPosition;
    }

#if CAUSTICA_WITH_STREAMLINE
    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS)
        m_bindings.gpuDevice.GetStreamline().ReflexTriggerFlash(m_bindings.gpuDevice.GetFrameIndex());
#endif

    return true;
}

bool PathTracerInputController::onMouseScrollEvent(double xoffset, double yoffset)
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddMouseWheelEvent(static_cast<float>(xoffset), static_cast<float>(yoffset));

    if (io.WantCaptureMouse)
        return true;

    GameScene* game = m_bindings.sampleGame;
    if (!(game && game->CameraActive()))
        m_bindings.settings.CameraMoveSpeed *= 1.0f + static_cast<float>(yoffset) * 0.1f;

    return true;
}

#pragma once

#include <events/Event.h>

namespace caustica
{
class GpuDevice;
class RenderCore;
}

class ZoomTool;
class GameScene;

namespace caustica::editor
{
class EditorUIState;
}

namespace caustica::render
{
class PathTracingWorldRenderer;
struct RenderSessionState;
}

namespace caustica::editor
{

// Routes keyboard/mouse events to camera, game scene, zoom tool, and editor toggles.
class EditorInputRouter
{
public:
    struct Context
    {
        caustica::render::RenderSessionState& session;
        EditorUIState& editor;
        caustica::RenderCore* renderCore = nullptr;
        caustica::GpuDevice* gpuDevice = nullptr;
        caustica::render::PathTracingWorldRenderer* worldRenderer = nullptr;
        ZoomTool* zoomTool = nullptr;
        GameScene* game = nullptr;
    };

    explicit EditorInputRouter(Context context);

    void updateContext(Context context)
    {
        m_ctx.renderCore = context.renderCore;
        m_ctx.gpuDevice = context.gpuDevice;
        m_ctx.worldRenderer = context.worldRenderer;
        m_ctx.zoomTool = context.zoomTool;
        m_ctx.game = context.game;
    }

    void onEvent(caustica::Event& event);

private:
    Context m_ctx;
};

} // namespace caustica::editor

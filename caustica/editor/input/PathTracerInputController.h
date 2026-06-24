#pragma once

#include <platform/Input.h>
#include <math/math.h>
#include <functional>

class GameScene;
class ZoomTool;
struct EditorUIState;
struct PathTracerSettings;

namespace caustica
{
class GpuDevice;
class RenderCore;
}

// =============================================================================
// PathTracerInputController — Editor layer: keyboard/mouse input for the path
// tracer sample. Handles camera control, editor shortcuts, picking requests,
// and optional game/zoom-tool overlays. Registered with GpuDevice::Input.
// =============================================================================
class PathTracerInputController : public caustica::IInputHandler
{
public:
    struct Bindings
    {
        caustica::GpuDevice&   gpuDevice;
        caustica::RenderCore&  renderCore;
        PathTracerSettings&    settings;
        EditorUIState&         editor;
        GameScene*             sampleGame = nullptr;
        std::function<ZoomTool*()> getZoomTool;
        std::function<dm::float2()> getUpscalingScale;
    };

    explicit PathTracerInputController(Bindings bindings);

    bool onKeyEvent(int key, int scancode, int action, int mods) override;
    bool onMouseMoveEvent(double xpos, double ypos) override;
    bool onMouseButtonEvent(int button, int action, int mods) override;
    bool onMouseScrollEvent(double xoffset, double yoffset) override;

private:
    Bindings m_bindings;
};

#pragma once

#include <events/event.h>
#include <events/key_event.h>
#include <events/mouse_event.h>
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
// PathTracerInputController — Editor layer: receives events via onEvent(Event&)
// and dispatches keyboard/mouse input to camera, editor shortcuts, picking,
// and optional game/zoom-tool overlays.
//
// Called from EditorApplication::onEvent() — the single event entry point.
// =============================================================================
class PathTracerInputController
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

    // Single entry point for all input events.
    void onEvent(caustica::Event& event);

private:
    bool onKeyPressed(caustica::KeyPressedEvent& e);
    bool onKeyReleased(caustica::KeyReleasedEvent& e);
    bool onMouseMoved(caustica::MouseMovedEvent& e);
    bool onMouseButtonPressed(caustica::MouseButtonPressedEvent& e);
    bool onMouseButtonReleased(caustica::MouseButtonReleasedEvent& e);
    bool onMouseScrolled(caustica::MouseScrolledEvent& e);

    Bindings m_bindings;
};

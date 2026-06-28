#pragma once

#include <render/WorldRenderer/PathTracingFrameExtension.h>

namespace caustica::editor
{

class SceneEditor;

// Editor-side frame extension: capture scripts, pick feedback, zoom overlay, etc.
// Keeps editor-only tooling out of the engine renderer.
class SceneEditorFrameExtension : public render::IPathTracingFrameExtension
{
public:
    explicit SceneEditorFrameExtension(SceneEditor& sceneEditor);

    void onFrameEvent(render::PathTracingFrameEvent& event) override;

private:
    SceneEditor& m_sceneEditor;
};

} // namespace caustica::editor

#pragma once

#include <render/WorldRenderer/PathTracingFramePipeline.h>

namespace caustica::editor
{

class SceneEditor;

// Editor-side frame pass: capture scripts, pick feedback, zoom overlay, etc.
class SceneEditorFrameExtension : public render::IPathTracingFramePass
{
public:
    explicit SceneEditorFrameExtension(SceneEditor& sceneEditor);

    [[nodiscard]] const char* name() const override { return "SceneEditor"; }
    void execute(render::PathTracingFrameContext& ctx) override;

private:
    SceneEditor& m_sceneEditor;
};

} // namespace caustica::editor

#include "SceneEditorFrameExtension.h"

#include "SceneEditor.h"

#include <render/Passes/Debug/ZoomTool.h>

namespace caustica::editor
{

SceneEditorFrameExtension::SceneEditorFrameExtension(SceneEditor& sceneEditor)
    : m_sceneEditor(sceneEditor)
{
}

void SceneEditorFrameExtension::execute(render::PathTracingFrameContext& ctx)
{
    switch (ctx.framePhase)
    {
    case render::PathTracingFramePhase::PreRender:
        m_sceneEditor.CaptureScriptPreRender();
        break;

    case render::PathTracingFramePhase::IdleMaintenance:
        m_sceneEditor.CollectUncompressedTextures();
        break;

    case render::PathTracingFramePhase::BeforePathTrace:
        ctx.pathTraceDebug.exploreDeltaTree = m_sceneEditor.ShowDeltaTree();
        break;

    case render::PathTracingFramePhase::BeforeFinalBlit:
        if (ctx.commandList && ctx.ldrColor)
        {
            if (ZoomTool* zoom = m_sceneEditor.GetOrCreateZoomTool())
                zoom->Render(ctx.commandList, ctx.ldrColor);
        }
        break;

    case render::PathTracingFramePhase::AfterPickResolved:
        if (ctx.pickFeedback)
            m_sceneEditor.ResolvePickFeedback(*ctx.pickFeedback);
        break;

    case render::PathTracingFramePhase::PostRender:
        if (ctx.postRender.saveFramebuffer)
        {
            m_sceneEditor.CaptureScriptPostRender(
                [save = ctx.postRender.saveFramebuffer](const char* fileName) {
                    return (*save)(fileName);
                });
        }
        if (m_sceneEditor.ConsumeExperimentalPhotoScreenshot())
            ctx.postRender.experimentalScreenshotRequested = true;
        break;

    default:
        break;
    }
}

} // namespace caustica::editor

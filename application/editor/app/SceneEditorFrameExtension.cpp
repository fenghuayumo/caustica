#include "SceneEditorFrameExtension.h"

#include "SceneEditor.h"

#include <render/Passes/Debug/ZoomTool.h>

namespace caustica::editor
{

SceneEditorFrameExtension::SceneEditorFrameExtension(SceneEditor& sceneEditor)
    : m_sceneEditor(sceneEditor)
{
}

void SceneEditorFrameExtension::onFrameEvent(render::PathTracingFrameEvent& event)
{
    switch (event.framePhase)
    {
    case render::PathTracingFramePhase::PreRender:
        m_sceneEditor.CaptureScriptPreRender();
        break;

    case render::PathTracingFramePhase::IdleMaintenance:
        m_sceneEditor.CollectUncompressedTextures();
        break;

    case render::PathTracingFramePhase::BeforePathTrace:
        if (event.pathTraceDebug)
            event.pathTraceDebug->exploreDeltaTree = m_sceneEditor.ShowDeltaTree();
        break;

    case render::PathTracingFramePhase::BeforeFinalBlit:
        if (event.commandList && event.ldrColor)
        {
            if (ZoomTool* zoom = m_sceneEditor.GetOrCreateZoomTool())
                zoom->Render(event.commandList, event.ldrColor);
        }
        break;

    case render::PathTracingFramePhase::AfterPickResolved:
        if (event.pickFeedback)
            m_sceneEditor.ResolvePickFeedback(*event.pickFeedback);
        break;

    case render::PathTracingFramePhase::PostRender:
        if (event.postRender)
        {
            if (event.postRender->saveFramebuffer)
            {
                m_sceneEditor.CaptureScriptPostRender(
                    [save = event.postRender->saveFramebuffer](const char* fileName) {
                        return (*save)(fileName);
                    });
            }
            if (m_sceneEditor.ConsumeExperimentalPhotoScreenshot())
                event.postRender->experimentalScreenshotRequested = true;
        }
        break;
    }
}

} // namespace caustica::editor

#include "PathTracerFrameDriver.h"

#include "EditorSessionHost.h"
#include "SceneEditor.h"

#include <engine/EngineRenderer.h>
#include <backend/GpuDevice.h>

namespace caustica::editor
{

PathTracerFrameDriver::PathTracerFrameDriver(caustica::GpuDevice* gpuDevice,
    caustica::Window* window,
    SceneEditor* sceneEditor,
    caustica::EngineRenderer* engineRenderer)
    : caustica::Application(gpuDevice, window)
    , m_sceneEditor(sceneEditor)
    , m_engineRenderer(engineRenderer)
{
}

void PathTracerFrameDriver::syncToBackBuffer()
{
    caustica::GpuDevice* gpuDevice = getGpuDevice();
    if (gpuDevice)
        syncPathTracerSessionBackBuffer(*gpuDevice, *this);
}

void PathTracerFrameDriver::onBeginFrame(caustica::GpuDevice& /*gpuDevice*/)
{
    if (m_sceneEditor)
        m_sceneEditor->beginFrame();
}

bool PathTracerFrameDriver::skipRenderPhase() const
{
    return m_sceneEditor && m_sceneEditor->shouldSkipRender();
}

void PathTracerFrameDriver::onUpdate(float elapsedTimeSeconds, bool windowFocused)
{
    if (m_sceneEditor && windowFocused)
        m_sceneEditor->Animate(elapsedTimeSeconds);
}

void PathTracerFrameDriver::onRender()
{
    caustica::GpuDevice* gpuDevice = getGpuDevice();
    if (!m_sceneEditor || !gpuDevice || m_sceneEditor->shouldSkipRender())
        return;

    m_sceneEditor->Render(gpuDevice->GetCurrentFramebuffer(true));

    if (m_engineRenderer)
        m_engineRenderer->endFrame();
}

bool PathTracerFrameDriver::shouldRenderWhenUnfocused() const
{
    return m_sceneEditor && m_sceneEditor->ShouldRenderUnfocused();
}

void PathTracerFrameDriver::onBackBufferResizing()
{
    if (m_sceneEditor)
        m_sceneEditor->BackBufferResizing();
}

} // namespace caustica::editor

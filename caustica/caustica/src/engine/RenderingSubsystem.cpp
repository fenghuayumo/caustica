#include <engine/RenderingSubsystem.h>

#include <backend/GpuDevice.h>

namespace caustica
{

void RenderingSubsystem::initialize(EngineInitContext& context)
{
    if (!context.gpuDevice)
        return;

    if (!m_renderer)
        m_renderer = std::make_unique<EngineRenderer>();
}

void RenderingSubsystem::shutdown()
{
    if (m_renderer)
    {
        m_renderer->shutdown();
        m_renderer.reset();
    }
}

void RenderingSubsystem::onRenderEnd(GpuDevice& /*gpuDevice*/)
{
    if (m_renderer)
        m_renderer->endFrame();
}

bool RenderingSubsystem::initializeRenderer(GpuDevice& gpuDevice,
    std::shared_ptr<SceneTypeFactory> sceneTypeFactory,
    EngineSceneCallbacks sceneCallbacks)
{
    if (!m_renderer)
        m_renderer = std::make_unique<EngineRenderer>();

    return m_renderer->initialize(gpuDevice, std::move(sceneTypeFactory), std::move(sceneCallbacks));
}

void RenderingSubsystem::createPathTracer(const PathTracerSessionParams& session)
{
    if (m_renderer)
        m_renderer->createPathTracer(session);
}

} // namespace caustica

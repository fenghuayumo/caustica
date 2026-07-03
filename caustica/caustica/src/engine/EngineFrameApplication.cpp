#include <engine/EngineFrameApplication.h>

#include <backend/GpuDevice.h>

namespace caustica
{

EngineFrameApplication::EngineFrameApplication(Engine* engine, GpuDevice* gpuDevice, Window* window)
    : Application(gpuDevice, window)
    , m_engine(engine)
{
}

void EngineFrameApplication::syncSwapChain()
{
    GpuDevice* gpuDevice = getGpuDevice();
    if (gpuDevice)
        Engine::syncSwapChain(*gpuDevice, *this);
}

void EngineFrameApplication::onBeginFrame(GpuDevice& gpuDevice)
{
    if (m_engine)
        m_engine->onBeginFrame(gpuDevice);
}

bool EngineFrameApplication::skipRenderPhase() const
{
    return m_engine && m_engine->skipRenderPhase();
}

void EngineFrameApplication::onUpdate(float elapsedTimeSeconds, bool windowFocused)
{
    if (m_engine)
        m_engine->onUpdate(elapsedTimeSeconds, windowFocused);
}

void EngineFrameApplication::onRender()
{
    GpuDevice* gpuDevice = getGpuDevice();
    if (!m_engine || !gpuDevice)
        return;

    m_engine->onRenderScene(*gpuDevice);
    m_engine->onRenderEnd(*gpuDevice);
}

bool EngineFrameApplication::shouldRenderWhenUnfocused() const
{
    return m_engine && m_engine->shouldRenderWhenUnfocused();
}

void EngineFrameApplication::onBackBufferResizing()
{
    if (m_engine)
        m_engine->onBackBufferResizing();
}

} // namespace caustica

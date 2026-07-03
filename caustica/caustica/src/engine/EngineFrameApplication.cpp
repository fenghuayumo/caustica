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
    if (!m_engine || !gpuDevice || m_engine->skipRenderPhase())
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

void EngineFrameApplication::onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    if (m_engine)
        m_engine->onBackBufferResized(width, height, sampleCount);
}

} // namespace caustica

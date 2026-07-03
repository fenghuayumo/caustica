#include <engine/Engine.h>

#include <engine/Application.h>
#include <backend/GpuDevice.h>

namespace caustica
{

Engine::~Engine()
{
    shutdown();
}

void Engine::addSubsystem(std::unique_ptr<ISubsystem> subsystem)
{
    m_subsystems.add(std::move(subsystem));
}

bool Engine::initialize(EngineInitContext context)
{
    if (m_initialized)
        return true;

    if (!context.gpuDevice)
        return false;

    if (!m_subsystems.initializeAll(context))
        return false;

    m_initialized = true;
    return true;
}

void Engine::shutdown()
{
    if (!m_initialized)
        return;

    m_subsystems.shutdownAll();
    m_initialized = false;
}

void Engine::onBeginFrame(GpuDevice& gpuDevice)
{
    m_subsystems.onBeginFrame(gpuDevice);
}

void Engine::onUpdate(float elapsedTimeSeconds, bool windowFocused)
{
    m_subsystems.onUpdate(elapsedTimeSeconds, windowFocused);
}

void Engine::onRenderScene(GpuDevice& gpuDevice)
{
    m_subsystems.onRenderScene(gpuDevice);
}

void Engine::onRenderEnd(GpuDevice& gpuDevice)
{
    m_subsystems.onRenderEnd(gpuDevice);
}

void Engine::onBackBufferResizing()
{
    m_subsystems.onBackBufferResizing();
}

void Engine::onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    m_subsystems.onBackBufferResized(width, height, sampleCount);
}

bool Engine::skipRenderPhase() const
{
    return m_subsystems.skipRenderPhase();
}

bool Engine::shouldRenderWhenUnfocused() const
{
    return m_subsystems.shouldRenderWhenUnfocused();
}

void Engine::syncSwapChain(GpuDevice& gpuDevice, Application& frameDriver)
{
    if (!gpuDevice.GetDevice())
        return;

    const BackBufferInfo backBuffer = gpuDevice.GetBackBufferInfo();
    frameDriver.notifyBackBufferResizing();
    frameDriver.notifyBackBufferResized(backBuffer.width, backBuffer.height, backBuffer.sampleCount);
}

} // namespace caustica

#include <engine/Engine.h>

#include <engine/App.h>
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

void Engine::onBackBufferResizing()
{
    m_subsystems.onBackBufferResizing();
}

void Engine::onBackBufferResized(uint32_t width, uint32_t height, uint32_t sampleCount)
{
    m_subsystems.onBackBufferResized(width, height, sampleCount);
}

void Engine::syncSwapChain(GpuDevice& gpuDevice, App& app)
{
    if (!gpuDevice.GetDevice())
        return;

    const BackBufferInfo backBuffer = gpuDevice.GetBackBufferInfo();
    app.notifyBackBufferResizing();
    app.notifyBackBufferResized(backBuffer.width, backBuffer.height, backBuffer.sampleCount);
}

} // namespace caustica

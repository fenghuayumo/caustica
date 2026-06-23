#include "render/renderer.h"

// nvrhi headers
#include <rhi/nvrhi.h>

// existing engine
#include <engine/Scene.h>
#include <engine/SceneGraph.h>

#include <cstdio>

namespace caustica
{

Renderer::Renderer(nvrhi::IDevice* device, nvrhi::ISwapchain* swapchain)
    : m_Device(device)
    , m_Swapchain(swapchain)
{
    fprintf(stdout, "[Renderer] Created\n");
}

Renderer::~Renderer()
{
    shutdown();
    fprintf(stdout, "[Renderer] Destroyed\n");
}

bool Renderer::init(std::array<uint32_t, 2> swapchainExtent)
{
    m_RenderExtent = swapchainExtent;
    m_Initialized  = true;

    fprintf(stdout, "[Renderer] Initialized: %ux%u\n",
        swapchainExtent[0], swapchainExtent[1]);
    return true;
}

void Renderer::shutdown()
{
    stopRenderThread();
    m_Initialized = false;
}

// ---------------------------------------------------------------------------
// Frame packet — game thread
// ---------------------------------------------------------------------------
RenderFramePacket Renderer::buildFramePacket(float deltaTime, std::array<uint32_t, 2> extent)
{
    RenderFramePacket packet;
    packet.deltaTime    = deltaTime;
    packet.frameIndex   = m_FrameIndex;
    packet.renderExtent = extent;
    packet.scene        = m_CurrentScene;
    return packet;
}

void Renderer::enqueueFramePacket(RenderFramePacket&& packet)
{
    {
        std::lock_guard<std::mutex> lock(m_FrameMutex);
        if (m_PendingFrames.size() >= kMaxPendingFrames)
            m_PendingFrames.erase(m_PendingFrames.begin());
        m_PendingFrames.push_back(std::move(packet));
    }
    m_FrameCV.notify_one();
}

void Renderer::consumeFramePacket(RenderFramePacket&& packet)
{
    // This runs on the render thread.
    // In Phase D, this will execute the actual render passes.
    // For now, just increment the frame counter and log.
    m_FrameIndex = packet.frameIndex + 1;

    // Placeholder: actual pass execution coming in Phase D
    // if (m_GBufferPass) m_GBufferPass->execute(...);
    // if (m_RTXDI)       m_RTXDI->execute(...);
    // ...
}

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------
void Renderer::startRenderThread()
{
    if (m_RenderThreadRunning.load())
        return;

    m_RenderThreadStopRequested = false;
    m_RenderThreadRunning.store(true);
    m_RenderThread = std::thread(&Renderer::renderThreadMain, this);
    fprintf(stdout, "[Renderer] Render thread started\n");
}

void Renderer::stopRenderThread()
{
    if (!m_RenderThreadRunning.load())
        return;

    m_RenderThreadStopRequested = true;
    m_FrameCV.notify_all();

    if (m_RenderThread.joinable())
        m_RenderThread.join();

    m_RenderThreadRunning.store(false);
    fprintf(stdout, "[Renderer] Render thread stopped\n");
}

void Renderer::waitForRenderIdle()
{
    if (!m_RenderThreadRunning.load())
        return;

    // Simple barrier: wait until pending frames are consumed
    while (true)
    {
        std::lock_guard<std::mutex> lock(m_FrameMutex);
        if (m_PendingFrames.empty())
            break;
    }
}

void Renderer::renderThreadMain()
{
    fprintf(stdout, "[Renderer] Render thread running\n");

    while (!m_RenderThreadStopRequested)
    {
        // 1. Flush pending commands
        {
            std::lock_guard<std::mutex> lock(m_CommandMutex);
            for (auto& cmd : m_PendingCommands)
                cmd();
            m_PendingCommands.clear();
        }

        // 2. Wait for a frame packet
        RenderFramePacket packet;
        bool hasPacket = false;
        {
            std::unique_lock<std::mutex> lock(m_FrameMutex);
            m_FrameCV.wait(lock, [this] {
                return !m_PendingFrames.empty() || m_RenderThreadStopRequested;
            });

            if (!m_PendingFrames.empty())
            {
                packet    = std::move(m_PendingFrames.front());
                m_PendingFrames.erase(m_PendingFrames.begin());
                hasPacket = true;
            }
        }

        // 3. Execute the frame
        if (hasPacket)
            consumeFramePacket(std::move(packet));
    }

    fprintf(stdout, "[Renderer] Render thread exiting\n");
}

// ---------------------------------------------------------------------------
// Thread-safe command queue
// ---------------------------------------------------------------------------
void Renderer::enqueueRenderCommand(std::function<void()>&& cmd)
{
    std::lock_guard<std::mutex> lock(m_CommandMutex);
    m_PendingCommands.push_back(std::move(cmd));
}

void Renderer::flushRenderCommands()
{
    // Commands are flushed at the start of each render thread iteration.
    // For immediate flush, we'd need to wake the render thread.
    m_FrameCV.notify_all();
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------
void Renderer::handleResize(uint32_t width, uint32_t height)
{
    m_RenderExtent = {width, height};
    fprintf(stdout, "[Renderer] Resize to %ux%u\n", width, height);
    // Phase D: recreate render targets, notify passes
}

} // namespace caustica

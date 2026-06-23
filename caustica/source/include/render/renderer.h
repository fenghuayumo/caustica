/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*/

#pragma once

#include <memory>
#include <functional>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <array>
#include <cstdint>

// Forward: nvrhi
namespace nvrhi {
    class IDevice;
    class ISwapchain;
    class IFramebuffer;
    class ITexture;
    class ICommandList;
}

// Forward: caustica existing
namespace caustica {
    class Scene;
    class SceneGraph;
    class ShaderFactory;
    class CommonRenderPasses;
    class TextureCache;
    class BindingCache;
    class DescriptorTableManager;
}

namespace caustica {

// =============================================================================
// RenderFramePacket — data snapshot for one frame rendered on the render thread.
// =============================================================================
struct RenderFramePacket
{
    float                deltaTime = 0.0f;
    uint32_t             frameIndex = 0;
    std::array<uint32_t, 2> renderExtent = {0, 0};
    Scene*               scene = nullptr;
    bool                 resetAccumulation = false;

    // Per-frame camera / view data (to be expanded in Phase D)
    // CameraMatrices cameraMatrices;
};

// =============================================================================
// Renderer — owns GPU resources and orchestrates render passes.
// Runs a dedicated render thread. Game thread builds frame packets;
// render thread consumes and executes them.
// =============================================================================
class Renderer
{
public:
    Renderer(nvrhi::IDevice* device, nvrhi::ISwapchain* swapchain);
    virtual ~Renderer();

    // --- Lifecycle ---
    virtual bool init(std::array<uint32_t, 2> swapchainExtent);
    virtual void shutdown();

    // --- Frame packet interface ---
    // Called on game thread: build a frame packet from current state
    virtual RenderFramePacket buildFramePacket(float deltaTime, std::array<uint32_t, 2> extent);

    // Called on game thread: enqueue a packet for the render thread
    virtual void enqueueFramePacket(RenderFramePacket&& packet);

    // Called on render thread: consume and execute one frame packet
    virtual void consumeFramePacket(RenderFramePacket&& packet);

    // --- Render thread ---
    void startRenderThread();
    void stopRenderThread();
    void waitForRenderIdle();
    bool isRenderThreadRunning() const { return m_RenderThreadRunning.load(std::memory_order_acquire); }

    // --- Render commands (thread-safe) ---
    void enqueueRenderCommand(std::function<void()>&& cmd);
    void flushRenderCommands();

    // --- Window / resize ---
    virtual void handleResize(uint32_t width, uint32_t height);

    // --- Scene ---
    void setCurrentScene(Scene* scene) { m_CurrentScene = scene; }
    Scene* getCurrentScene() const { return m_CurrentScene; }

    // --- Accessors ---
    nvrhi::IDevice*      getDevice()    const { return m_Device; }
    nvrhi::ISwapchain*   getSwapchain() const { return m_Swapchain; }
    uint32_t             getFrameIndex() const { return m_FrameIndex; }

    // --- Passes (to be populated from caustica.cpp in Phase D) ---
    // These are stored here so the Renderer owns their lifetime.
    // Actual pass creation moves from Sample::CreateRenderPasses() later.

protected:
    // --- Render thread main loop ---
    void renderThreadMain();

    // --- GPU resources ---
    nvrhi::IDevice*      m_Device    = nullptr;
    nvrhi::ISwapchain*   m_Swapchain = nullptr;

    // --- Scene reference ---
    Scene* m_CurrentScene = nullptr;

    // --- Render thread ---
    std::thread m_RenderThread;
    std::atomic<bool> m_RenderThreadRunning{false};
    bool m_RenderThreadStopRequested = false;

    std::mutex m_FrameMutex;
    std::condition_variable m_FrameCV;
    std::vector<RenderFramePacket> m_PendingFrames;
    static constexpr size_t kMaxPendingFrames = 2;

    // --- Command queue ---
    std::mutex m_CommandMutex;
    std::vector<std::function<void()>> m_PendingCommands;

    // --- State ---
    uint32_t m_FrameIndex = 0;
    std::array<uint32_t, 2> m_RenderExtent = {0, 0};
    bool m_Initialized = false;
};

} // namespace caustica

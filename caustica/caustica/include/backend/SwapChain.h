#pragma once

#include <vector>
#include <memory>
#include <cstdint>

namespace nvrhi {
    class IDevice;
    class ISwapchain;
    class IFramebuffer;
    class ITexture;
    class Format;
}

namespace caustica {

// =============================================================================
// SwapChain — Backend layer: swapchain + framebuffer management.
//
// Extracted from DeviceManager. Owns the NVRHI swapchain and all
// back-buffer framebuffers (with and without depth attachment).
// Handles resize, begin/end frame, present.
// =============================================================================
class SwapChain
{
public:
    SwapChain() = default;
    ~SwapChain();

    // --- Lifecycle ---
    bool create(nvrhi::IDevice* device, void* nativeWindow,
                uint32_t width, uint32_t height,
                uint32_t bufferCount, nvrhi::Format format,
                uint32_t sampleCount = 1, uint32_t sampleQuality = 0,
                nvrhi::Format depthFormat = nvrhi::Format::UNKNOWN,
                bool vsync = true);
    void shutdown();

    // --- Resize ---
    bool resize(uint32_t width, uint32_t height);

    // --- Per-frame ---
    bool beginFrame();
    bool present();

    // --- Framebuffer access ---
    nvrhi::IFramebuffer* getFramebuffer(uint32_t index, bool withDepth = false);
    nvrhi::IFramebuffer* getCurrentFramebuffer(bool withDepth = false);
    uint32_t             getCurrentBackBufferIndex() const { return m_CurrentBackBufferIndex; }
    uint32_t             getBackBufferCount() const        { return m_BufferCount; }

    // --- Depth buffer ---
    nvrhi::ITexture*     getDepthBuffer() const { return m_DepthBuffer.get(); }

    // --- Resize event (called when back buffer resizes) ---
    using ResizeCallback = std::function<void()>;
    void setResizeCallback(ResizeCallback cb) { m_ResizeCallback = std::move(cb); }

    // --- State ---
    bool isValid() const { return m_Swapchain != nullptr; }
    uint32_t width() const  { return m_Width; }
    uint32_t height() const { return m_Height; }

private:
    bool createDepthBuffer(nvrhi::IDevice* device, nvrhi::Format format);
    bool createFramebuffers(nvrhi::IDevice* device);

    nvrhi::ISwapchain* m_Swapchain = nullptr;
    uint32_t m_BufferCount = 3;
    uint32_t m_Width = 0;
    uint32_t m_Height = 0;
    uint32_t m_SampleCount = 1;
    uint32_t m_SampleQuality = 0;
    bool     m_VSync = true;
    uint32_t m_CurrentBackBufferIndex = 0;

    std::shared_ptr<nvrhi::ITexture> m_DepthBuffer;
    std::vector<std::shared_ptr<nvrhi::IFramebuffer>> m_Framebuffers;
    std::vector<std::shared_ptr<nvrhi::IFramebuffer>> m_FramebuffersWithDepth;

    ResizeCallback m_ResizeCallback;
};

} // namespace caustica

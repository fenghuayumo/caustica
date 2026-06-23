#pragma once

#include <string>
#include <vector>
#include <list>
#include <rhi/nvrhi.h>

namespace caustica {

class IRenderPass;
struct DeviceCreationParameters;
struct AdapterInfo;

// =============================================================================
// GpuDevice — Backend layer: GPU adapter/device/swapchain lifecycle.
// Platform-specific subclasses (GpuDevice_DX12/VK/DX11) implement the
// virtual methods. Replaces DeviceManager's GPU responsibilities.
// =============================================================================
class GpuDevice
{
public:
    GpuDevice() = default;
    virtual ~GpuDevice() = default;

    // --- Factory ---
    static GpuDevice* create(nvrhi::GraphicsAPI api);

    // --- Instance / Device / SwapChain ---
    bool createInstance(const DeviceCreationParameters& params);
    bool createDeviceAndSwapChain(const DeviceCreationParameters& params,
                                   void* windowHandle, const char* windowTitle);
    bool createHeadlessDevice(const DeviceCreationParameters& params);
    void shutdown();

    // --- Accessors ---
    nvrhi::IDevice*             getDevice() const          { return m_NvrhiDevice; }
    const DeviceCreationParameters& getParams() const      { return *m_Params; }
    bool                        isCreated() const          { return m_NvrhiDevice != nullptr; }

    // --- Render passes ---
    void addRenderPassToFront(IRenderPass* p);
    void addRenderPassToBack(IRenderPass* p);
    void removeRenderPass(IRenderPass* p);
    void backBufferResizing();
    void backBufferResized();

    // --- Framebuffers ---
    nvrhi::IFramebuffer* getCurrentFramebuffer(bool withDepth = true);
    nvrhi::IFramebuffer* getFramebuffer(uint32_t index, bool withDepth = true);
    nvrhi::ITexture*     getDepthBuffer() const { return m_DepthBuffer; }

    // --- Platform-specific virtuals ---
    virtual bool createInstanceInternal() { return false; }
    virtual bool createDevice() { return false; }
    virtual bool enumerateAdapters(std::vector<AdapterInfo>&) { return false; }
    virtual const char* getRendererString() const { return "Unknown"; }
    virtual nvrhi::GraphicsAPI getGraphicsAPI() const { return nvrhi::GraphicsAPI::D3D12; }

    // Swapchain
    virtual bool createSwapChain() { return false; }
    virtual void destroyDeviceAndSwapChain() {}
    virtual void resizeSwapChain() {}
    virtual bool beginFrame() { return false; }
    virtual bool present() { return false; }
    virtual nvrhi::ITexture* getCurrentBackBuffer() { return nullptr; }
    virtual nvrhi::ITexture* getBackBuffer(uint32_t) { return nullptr; }
    virtual uint32_t getCurrentBackBufferIndex() { return 0; }
    virtual uint32_t getBackBufferCount() { return 0; }

    virtual void setVsyncEnabled(bool) {}
    virtual void reportLiveObjects() {}

protected:
    nvrhi::IDevice*    m_NvrhiDevice = nullptr;
    const DeviceCreationParameters* m_Params = nullptr;
    bool m_InstanceCreated = false;

    void*     m_Window = nullptr;            // GLFWwindow* or HWND
    std::string m_WindowTitle;

    // Framebuffer state (populated by platform CreateSwapChain)
    std::vector<nvrhi::FramebufferHandle> m_Framebuffers;
    std::vector<nvrhi::FramebufferHandle> m_FramebuffersWithDepth;
    nvrhi::TextureHandle m_DepthBuffer;

    // Headless
    std::vector<nvrhi::TextureHandle> m_HeadlessBackBuffers;
    uint32_t m_HeadlessBackBufferIndex = 0;

    // Render pass list
    std::list<IRenderPass*> m_RenderPasses;
    uint32_t m_CurrentBackBufferIndex = 0;

    void createDepthBuffer();
    bool createHeadlessBackBuffers();
    void releaseHeadlessBackBuffers();
    bool beginHeadlessFrame();
    bool presentHeadlessFrame();
};

} // namespace caustica

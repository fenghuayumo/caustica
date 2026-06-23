#include "backend/GpuDevice.h"
#include "engine/DeviceManager.h"  // DeviceCreationParameters, IRenderPass
#include <core/log.h>

namespace caustica {

bool GpuDevice::createInstance(const DeviceCreationParameters& params)
{
    if (m_InstanceCreated) return true;
    m_Params = &params;

    if (!params.headlessDevice)
    {
        if (!glfwInit())
            return false;
    }
    m_InstanceCreated = createInstanceInternal();
    return m_InstanceCreated;
}

bool GpuDevice::createDeviceAndSwapChain(const DeviceCreationParameters& params,
                                          void* windowHandle, const char* windowTitle)
{
    m_Params = &params;
    m_Window = windowHandle;
    if (windowTitle) m_WindowTitle = windowTitle;

    if (!createInstance(params))
        { caustica::error("GpuDevice: createInstance failed"); return false; }
    if (!createDevice())
        { caustica::error("GpuDevice: createDevice failed"); return false; }
    if (!createSwapChain())
        { caustica::error("GpuDevice: createSwapChain failed"); return false; }
    return true;
}

bool GpuDevice::createHeadlessDevice(const DeviceCreationParameters& params)
{
    auto p = params; p.headlessDevice = true;
    return createDeviceAndSwapChain(p, nullptr, nullptr);
}

void GpuDevice::shutdown()
{
    destroyDeviceAndSwapChain();
    m_NvrhiDevice = nullptr;
    m_InstanceCreated = false;
}

// --- Render pass management ---
void GpuDevice::addRenderPassToFront(IRenderPass* p)
{
    m_RenderPasses.remove(p);
    m_RenderPasses.push_front(p);
    p->BackBufferResizing();
    p->BackBufferResized(m_Params ? m_Params->backBufferWidth : 0,
                         m_Params ? m_Params->backBufferHeight : 0,
                         m_Params ? m_Params->swapChainSampleCount : 1);
}
void GpuDevice::addRenderPassToBack(IRenderPass* p)
{
    m_RenderPasses.remove(p);
    m_RenderPasses.push_back(p);
    p->BackBufferResizing();
    p->BackBufferResized(m_Params ? m_Params->backBufferWidth : 0,
                         m_Params ? m_Params->backBufferHeight : 0,
                         m_Params ? m_Params->swapChainSampleCount : 1);
}
void GpuDevice::removeRenderPass(IRenderPass* p) { m_RenderPasses.remove(p); }
void GpuDevice::backBufferResizing()
{
    m_Framebuffers.clear();
    m_FramebuffersWithDepth.clear();
    for (auto* p : m_RenderPasses) p->BackBufferResizing();
}
void GpuDevice::backBufferResized()
{
    createDepthBuffer();
    for (auto* p : m_RenderPasses)
        p->BackBufferResized(m_Params->backBufferWidth, m_Params->backBufferHeight, m_Params->swapChainSampleCount);
}

// --- Framebuffers ---
nvrhi::IFramebuffer* GpuDevice::getCurrentFramebuffer(bool withDepth)
    { return getFramebuffer(m_CurrentBackBufferIndex, withDepth); }
nvrhi::IFramebuffer* GpuDevice::getFramebuffer(uint32_t idx, bool withDepth)
{
    if (withDepth) return idx < m_FramebuffersWithDepth.size() ? m_FramebuffersWithDepth[idx].Get() : nullptr;
    else          return idx < m_Framebuffers.size()          ? m_Framebuffers[idx].Get()          : nullptr;
}

// --- Depth buffer ---
void GpuDevice::createDepthBuffer()
{
    m_DepthBuffer = nullptr;
    if (!m_Params || m_Params->depthBufferFormat == nvrhi::Format::UNKNOWN || !m_NvrhiDevice) return;

    auto desc = nvrhi::TextureDesc()
        .setDebugName("Depth Buffer")
        .setWidth(m_Params->backBufferWidth).setHeight(m_Params->backBufferHeight)
        .setFormat(m_Params->depthBufferFormat)
        .setDimension(m_Params->swapChainSampleCount > 1 ? nvrhi::TextureDimension::Texture2DMS : nvrhi::TextureDimension::Texture2D)
        .setSampleCount(m_Params->swapChainSampleCount).setSampleQuality(m_Params->swapChainSampleQuality)
        .setIsTypeless(true).setIsRenderTarget(true)
        .enableAutomaticStateTracking(nvrhi::ResourceStates::DepthWrite);
    m_DepthBuffer = m_NvrhiDevice->createTexture(desc);
}

// --- Headless ---
bool GpuDevice::createHeadlessBackBuffers()
{
    if (!m_Params || !m_NvrhiDevice) return false;
    for (uint32_t i = 0; i < m_Params->swapChainBufferCount; i++)
    {
        auto desc = nvrhi::TextureDesc()
            .setDebugName("HeadlessBackBuffer").setWidth(m_Params->backBufferWidth).setHeight(m_Params->backBufferHeight)
            .setFormat(m_Params->swapChainFormat).setIsRenderTarget(true)
            .setInitialState(nvrhi::ResourceStates::RenderTarget).setKeepInitialState(true);
        m_HeadlessBackBuffers.push_back(m_NvrhiDevice->createTexture(desc));
    }
    m_HeadlessBackBufferIndex = 0;
    return true;
}
void GpuDevice::releaseHeadlessBackBuffers() { m_HeadlessBackBuffers.clear(); }
bool GpuDevice::beginHeadlessFrame()
{
    if (m_HeadlessBackBufferIndex >= m_HeadlessBackBuffers.size()) m_HeadlessBackBufferIndex = 0;
    return true;
}
bool GpuDevice::presentHeadlessFrame()
{
    m_HeadlessBackBufferIndex = (m_HeadlessBackBufferIndex + 1) % m_HeadlessBackBuffers.size();
    return true;
}

} // namespace caustica

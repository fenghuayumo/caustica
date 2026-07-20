#include "common/EditorViewport.h"

#include <backend/GpuDevice.h>
#include <core/log.h>

#include <algorithm>

namespace caustica::editor
{

void EditorViewport::clearRetired()
{
    m_retiredFramebuffer = nullptr;
    m_retiredColor = nullptr;
    m_retiredDepth = nullptr;
}

void EditorViewport::release()
{
    clearRetired();
    m_framebuffer = nullptr;
    m_color = nullptr;
    m_depth = nullptr;
    m_width = 0;
    m_height = 0;
}

void EditorViewport::flushRetired(caustica::GpuDevice& device)
{
    if (!m_retiredFramebuffer && !m_retiredColor && !m_retiredDepth)
        return;

    // Prior path-trace / ImGui frames may still reference the retired color on the GPU.
    if (nvrhi::IDevice* nvrhiDevice = device.getDevice())
        nvrhiDevice->waitForIdle();

    clearRetired();
}

void EditorViewport::ensureSize(caustica::GpuDevice& device, uint32_t width, uint32_t height)
{
    width = std::max(1u, width);
    height = std::max(1u, height);

    if (m_framebuffer && m_width == width && m_height == height)
        return;

    nvrhi::IDevice* nvrhiDevice = device.getDevice();
    if (!nvrhiDevice)
        return;

    // Drop any already-retired buffers (from a previous resize) before retiring the current ones.
    if (m_retiredFramebuffer || m_retiredColor || m_retiredDepth)
    {
        nvrhiDevice->waitForIdle();
        clearRetired();
    }

    // Defer destruction: update-thread ImGui capture still points at the old color this frame.
    m_retiredFramebuffer = m_framebuffer;
    m_retiredColor = m_color;
    m_retiredDepth = m_depth;
    m_framebuffer = nullptr;
    m_color = nullptr;
    m_depth = nullptr;
    m_width = 0;
    m_height = 0;

    // Match typical swapchain LDR target (sRGB) so tone-mapped blit looks correct in ImGui.
    nvrhi::TextureDesc colorDesc = nvrhi::TextureDesc()
        .setDebugName("EditorViewport.Color")
        .setWidth(width)
        .setHeight(height)
        .setFormat(nvrhi::Format::SRGBA8_UNORM)
        .setIsRenderTarget(true)
        .setInitialState(nvrhi::ResourceStates::RenderTarget)
        .setKeepInitialState(true);

    m_color = nvrhiDevice->createTexture(colorDesc);
    if (!m_color)
    {
        caustica::error("EditorViewport: failed to create color texture %ux%u", width, height);
        return;
    }

    nvrhi::TextureDesc depthDesc = nvrhi::TextureDesc()
        .setDebugName("EditorViewport.Depth")
        .setWidth(width)
        .setHeight(height)
        .setFormat(nvrhi::Format::D32)
        .setIsTypeless(true)
        .setIsRenderTarget(true)
        .enableAutomaticStateTracking(nvrhi::ResourceStates::DepthWrite);

    m_depth = nvrhiDevice->createTexture(depthDesc);
    if (!m_depth)
    {
        caustica::error("EditorViewport: failed to create depth texture %ux%u", width, height);
        m_color = nullptr;
        return;
    }

    nvrhi::FramebufferDesc fbDesc = nvrhi::FramebufferDesc()
        .addColorAttachment(m_color)
        .setDepthAttachment(m_depth);

    m_framebuffer = nvrhiDevice->createFramebuffer(fbDesc);
    if (!m_framebuffer)
    {
        caustica::error("EditorViewport: failed to create framebuffer %ux%u", width, height);
        m_color = nullptr;
        m_depth = nullptr;
        return;
    }

    m_width = width;
    m_height = height;
}

} // namespace caustica::editor

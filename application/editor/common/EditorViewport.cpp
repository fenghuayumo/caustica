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
    if (caustica::rhi::IDevice* rhiDevice = device.getDevice())
        rhiDevice->waitForIdle();

    clearRetired();
}

void EditorViewport::ensureSize(caustica::GpuDevice& device, uint32_t width, uint32_t height)
{
    width = std::max(1u, width);
    height = std::max(1u, height);

    if (m_framebuffer && m_width == width && m_height == height)
        return;

    caustica::rhi::IDevice* rhiDevice = device.getDevice();
    if (!rhiDevice)
        return;

    // Drop any already-retired buffers (from a previous resize) before retiring the current ones.
    if (m_retiredFramebuffer || m_retiredColor || m_retiredDepth)
    {
        rhiDevice->waitForIdle();
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
    caustica::rhi::TextureDesc colorDesc = caustica::rhi::TextureDesc()
        .setDebugName("EditorViewport.Color")
        .setWidth(width)
        .setHeight(height)
        .setFormat(caustica::rhi::Format::SRGBA8_UNORM)
        .setIsRenderTarget(true)
        .setInitialState(caustica::rhi::ResourceStates::RenderTarget)
        .setKeepInitialState(true);

    m_color = rhiDevice->createTexture(colorDesc);
    if (!m_color)
    {
        caustica::error("EditorViewport: failed to create color texture %ux%u", width, height);
        return;
    }

    caustica::rhi::TextureDesc depthDesc = caustica::rhi::TextureDesc()
        .setDebugName("EditorViewport.Depth")
        .setWidth(width)
        .setHeight(height)
        .setFormat(caustica::rhi::Format::D32)
        .setIsTypeless(true)
        .setIsRenderTarget(true)
        .enableAutomaticStateTracking(caustica::rhi::ResourceStates::DepthWrite);

    m_depth = rhiDevice->createTexture(depthDesc);
    if (!m_depth)
    {
        caustica::error("EditorViewport: failed to create depth texture %ux%u", width, height);
        m_color = nullptr;
        return;
    }

    caustica::rhi::FramebufferDesc fbDesc = caustica::rhi::FramebufferDesc()
        .addColorAttachment(m_color)
        .setDepthAttachment(m_depth);

    m_framebuffer = rhiDevice->createFramebuffer(fbDesc);
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

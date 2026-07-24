#pragma once

#include <rhi/rhi.h>
#include <cstdint>

namespace caustica
{
class GpuDevice;
}

namespace caustica::editor
{

// Offscreen color+depth framebuffer that WorldRenderer targets for the editor viewport.
class EditorViewport
{
public:
    void ensureSize(caustica::GpuDevice& device, uint32_t width, uint32_t height);
    // Drop textures retired by ensureSize after ImGui GPU submit has finished using them.
    void flushRetired(caustica::GpuDevice& device);
    void release();

    [[nodiscard]] caustica::rhi::IFramebuffer* framebuffer() const { return m_framebuffer.Get(); }
    [[nodiscard]] caustica::rhi::ITexture* colorTexture() const { return m_color.Get(); }
    [[nodiscard]] uint32_t width() const { return m_width; }
    [[nodiscard]] uint32_t height() const { return m_height; }
    [[nodiscard]] bool isValid() const { return m_framebuffer != nullptr && m_width > 0 && m_height > 0; }

private:
    void clearRetired();

    caustica::rhi::TextureHandle m_color;
    caustica::rhi::TextureHandle m_depth;
    caustica::rhi::FramebufferHandle m_framebuffer;
    // Previous size kept alive until flushRetired(): ImGui draw data captured on the
    // update thread still references the old color texture for the current frame.
    caustica::rhi::TextureHandle m_retiredColor;
    caustica::rhi::TextureHandle m_retiredDepth;
    caustica::rhi::FramebufferHandle m_retiredFramebuffer;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace caustica::editor

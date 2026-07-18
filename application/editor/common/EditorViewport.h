#pragma once

#include <rhi/nvrhi.h>
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
    void release();

    [[nodiscard]] nvrhi::IFramebuffer* framebuffer() const { return m_framebuffer.Get(); }
    [[nodiscard]] nvrhi::ITexture* colorTexture() const { return m_color.Get(); }
    [[nodiscard]] uint32_t width() const { return m_width; }
    [[nodiscard]] uint32_t height() const { return m_height; }
    [[nodiscard]] bool isValid() const { return m_framebuffer != nullptr && m_width > 0 && m_height > 0; }

private:
    nvrhi::TextureHandle m_color;
    nvrhi::TextureHandle m_depth;
    nvrhi::FramebufferHandle m_framebuffer;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace caustica::editor

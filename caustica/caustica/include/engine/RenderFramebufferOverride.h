#pragma once

#include <rhi/rhi.h>

namespace caustica
{

// Optional host override for WorldRenderer::render() target (e.g. editor viewport FB).
// When framebuffer is null, the swapchain back buffer is used.
struct RenderFramebufferOverride
{
    caustica::rhi::IFramebuffer* framebuffer = nullptr;
};

} // namespace caustica

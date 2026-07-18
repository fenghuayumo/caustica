#pragma once

#include <rhi/nvrhi.h>

namespace caustica
{

// Optional host override for WorldRenderer::render() target (e.g. editor viewport FB).
// When framebuffer is null, the swapchain back buffer is used.
struct RenderFramebufferOverride
{
    nvrhi::IFramebuffer* framebuffer = nullptr;
};

} // namespace caustica

#pragma once

#include <vector>
#include <rhi/rhi.h>

namespace caustica {

// =============================================================================
// SwapChain - Backend layer: swapchain framebuffers and depth buffer state.
// =============================================================================
struct SwapChain
{
    std::vector<caustica::rhi::FramebufferHandle> framebuffers;
    std::vector<caustica::rhi::FramebufferHandle> framebuffersWithDepth;
    caustica::rhi::TextureHandle depthBuffer;

    uint32_t currentBackBufferIndex = 0;
    uint32_t backBufferCount = 0;
    uint32_t sampleCount = 1;
};

} // namespace caustica

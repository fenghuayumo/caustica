#pragma once

#include <vector>
#include <rhi/nvrhi.h>

namespace caustica {

// =============================================================================
// SwapChain — Backend layer: swapchain framebuffers and depth buffer state.
// =============================================================================
struct SwapChain
{
    std::vector<nvrhi::FramebufferHandle> framebuffers;
    std::vector<nvrhi::FramebufferHandle> framebuffersWithDepth;
    nvrhi::TextureHandle depthBuffer;

    uint32_t currentBackBufferIndex = 0;
    uint32_t backBufferCount = 0;
    uint32_t sampleCount = 1;
};

} // namespace caustica

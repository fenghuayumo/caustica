#pragma once

#include <string>
#include <vector>
#include <rhi/nvrhi.h>

namespace caustica {

struct DeviceCreationParameters;
struct AdapterInfo;

// =============================================================================
// GpuDevice — Backend layer: holds NVRHI device pointer and common state.
//
// The platform-specific virtual methods (CreateInstanceInternal, CreateDevice,
// CreateSwapChain, etc.) remain in DeviceManager until GpuDevice_DX12/VK/DX11
// are migrated to directly implement GpuDevice's interface.
// =============================================================================
class GpuDevice
{
public:
    virtual ~GpuDevice() = default;

    static GpuDevice* create(nvrhi::GraphicsAPI api);

    // --- Lifecycle (common logic) ---
    bool createInstance(const DeviceCreationParameters& params);

    // --- Accessors ---
    nvrhi::IDevice*  device = nullptr;
    bool             instanceCreated = false;

    // Swapchain state (populated by platform CreateSwapChain)
    std::vector<nvrhi::FramebufferHandle> framebuffers;
    std::vector<nvrhi::FramebufferHandle> framebuffersWithDepth;
    nvrhi::TextureHandle depthBuffer;
    uint32_t currentBackBufferIndex = 0;
    uint32_t backBufferCount = 0;

    nvrhi::IFramebuffer* getCurrentFramebuffer(bool withDepth = true);
    nvrhi::IFramebuffer* getFramebuffer(uint32_t index, bool withDepth = true);
    nvrhi::ITexture*     getDepthBuffer() const { return depthBuffer; }

    GpuDevice() = default;

private:
    const DeviceCreationParameters* m_Params = nullptr;
};

} // namespace caustica

#pragma once

#include <string>
#include <vector>
#include <rhi/nvrhi.h>

namespace caustica {

struct DeviceCreationParameters;
struct AdapterInfo;

// =============================================================================
// GpuBackend — Backend layer: platform-specific GPU implementation interface.
// =============================================================================
class GpuBackend
{
public:
    virtual ~GpuBackend() = default;

    virtual bool createInstanceInternal() = 0;
    virtual bool createDevice(const DeviceCreationParameters& params) = 0;
    virtual bool enumerateAdapters(std::vector<AdapterInfo>& out) = 0;

    virtual nvrhi::IDevice*      getDevice() const = 0;
    virtual nvrhi::GraphicsAPI   getGraphicsAPI() const = 0;
    virtual const char*          getRendererString() const = 0;

    virtual bool createSwapChain() = 0;
    virtual void destroyDeviceAndSwapChain() = 0;
    virtual void resizeSwapChain() = 0;
    virtual bool beginFrame() = 0;
    virtual bool present() = 0;

    virtual nvrhi::ITexture* getCurrentBackBuffer() = 0;
    virtual nvrhi::ITexture* getBackBuffer(uint32_t index) = 0;
    virtual uint32_t         getCurrentBackBufferIndex() = 0;
    virtual uint32_t         getBackBufferCount() = 0;

    virtual void setVsyncEnabled(bool) {}
    virtual void reportLiveObjects() {}
};

} // namespace caustica

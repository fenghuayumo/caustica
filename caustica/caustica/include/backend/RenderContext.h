#pragma once

#include <rhi/nvrhi.h>
#include <cstdint>

namespace caustica
{

class GpuDevice;

// GPU device access for UI/scene render helpers (not a pass registry).
class RenderContext
{
public:
    explicit RenderContext(GpuDevice* device)
        : m_GpuDevice(device)
    { }

    virtual ~RenderContext() = default;

    [[nodiscard]] GpuDevice* GetGpuDevice() const { return m_GpuDevice; }
    [[nodiscard]] nvrhi::IDevice* getDevice() const;
    [[nodiscard]] uint32_t GetFrameIndex() const;

protected:
    GpuDevice* m_GpuDevice;
};

} // namespace caustica

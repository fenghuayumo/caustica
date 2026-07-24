#pragma once

#include <rhi/rhi.h>
#include <cstdint>

namespace caustica
{

class GpuDevice;

// GPU device access for UI/scene render helpers (not a pass registry).
class renderContext
{
public:
    explicit renderContext(GpuDevice* device)
        : m_GpuDevice(device)
    { }

    virtual ~renderContext() = default;

    [[nodiscard]] GpuDevice* getGpuDevice() const { return m_GpuDevice; }
    [[nodiscard]] caustica::rhi::Device* getDevice() const;
    [[nodiscard]] uint32_t getFrameIndex() const;

protected:
    GpuDevice* m_GpuDevice;
};

} // namespace caustica

#pragma once

#include <rhi/nvrhi.h>
#include <cstdint>

namespace caustica
{

class GpuDevice;

// =============================================================================
// RenderContext — GPU device access for scene/UI renderers.
//
// Not a pass registry: Application drives update/render directly.
// =============================================================================
class RenderContext
{
public:
    explicit RenderContext(GpuDevice* device)
        : m_GpuDevice(device)
    { }

    virtual ~RenderContext() = default;

    [[nodiscard]] GpuDevice* GetGpuDevice() const { return m_GpuDevice; }
    [[nodiscard]] nvrhi::IDevice* GetDevice() const;
    [[nodiscard]] uint32_t GetFrameIndex() const;

protected:
    GpuDevice* m_GpuDevice;
};

} // namespace caustica

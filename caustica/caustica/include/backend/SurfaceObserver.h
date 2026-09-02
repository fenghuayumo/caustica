#pragma once

#include <cstdint>

namespace caustica
{

// Swapchain / DPI can fire from the render thread, so observers cannot wait
// for a schedule. GpuSurface notifies these directly.
class ISurfaceObserver
{
public:
    virtual ~ISurfaceObserver() = default;

    virtual void onSurfaceResizing() {}
    virtual void onSurfaceResized(uint32_t width, uint32_t height, uint32_t sampleCount)
    {
        (void)width;
        (void)height;
        (void)sampleCount;
    }
    virtual void onDisplayScaleChanged(float scaleX, float scaleY)
    {
        (void)scaleX;
        (void)scaleY;
    }
};

} // namespace caustica

#pragma once

#include <rhi/nvrhi.h>
#include <cstdint>

namespace caustica
{

class GpuDevice;

// =============================================================================
// IRenderPass — Interface for all render passes.
//
// Each render pass is registered with a GpuDevice and receives per-frame
// callbacks for animation, rendering, and back-buffer lifecycle events.
//
// Input handling has been moved to IInputHandler (platform/Input.h).
// Render passes that need input should implement IInputHandler separately
// and register with Input::registerHandler().
// =============================================================================
class IRenderPass
{
public:
    explicit IRenderPass(GpuDevice* device)
        : m_GpuDevice(device)
    { }

    virtual ~IRenderPass() = default;

    virtual void SetLatewarpOptions() { }
    virtual bool ShouldAnimateUnfocused() { return false; }
    virtual bool ShouldRenderUnfocused() { return false; }

    // If this function returns 'true', and the device has a depth buffer
    // (DeviceCreationParameters::depthBufferFormat != UNKNOWN), Render()
    // will be called with a framebuffer that has a depth attachment.
    // Otherwise, the framebuffer will only have a color attachment —
    // which is useful for UI rendering.
    virtual bool SupportsDepthBuffer() { return true; }

    virtual void Render(nvrhi::IFramebuffer* framebuffer) { }
    virtual void Animate(float fElapsedTimeSeconds) { }
    virtual void BackBufferResizing() { }
    virtual void BackBufferResized(const uint32_t width, const uint32_t height, const uint32_t sampleCount) { }

    // Called before Animate() when a DPI change was detected
    virtual void DisplayScaleChanged(float scaleX, float scaleY) { }

    [[nodiscard]] GpuDevice* GetGpuDevice() const { return m_GpuDevice; }
    [[nodiscard]] nvrhi::IDevice* GetDevice() const;
    [[nodiscard]] uint32_t GetFrameIndex() const;

private:
    GpuDevice* m_GpuDevice;
};

} // namespace caustica

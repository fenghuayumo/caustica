#pragma once

// Presentation surface: platform window + swapchain acquire/present/resize.
// GpuDevice is the logical GPU; it does not own the window.

#include <backend/SurfaceObserver.h>

#include <cstdint>
#include <string>
#include <vector>

#include <memory>

namespace caustica
{

class GpuDevice;
class Window;

class GpuSurface
{
public:
    static std::unique_ptr<GpuSurface> createWindowed(GpuDevice& device, Window& window);
    static std::unique_ptr<GpuSurface> createHeadless(GpuDevice& device);
    // Swapchain already exists (injected device). Does not recreate it.
    static std::unique_ptr<GpuSurface> adopt(GpuDevice& device, Window* window);

    ~GpuSurface();

    GpuSurface(const GpuSurface&) = delete;
    GpuSurface& operator=(const GpuSurface&) = delete;

    [[nodiscard]] GpuDevice& device() { return m_device; }
    [[nodiscard]] const GpuDevice& device() const { return m_device; }
    [[nodiscard]] Window* window() const { return m_window; }

    void addObserver(ISurfaceObserver& observer);
    void removeObserver(ISurfaceObserver& observer);

    [[nodiscard]] bool acquireFrame();
    [[nodiscard]] bool presentFrame();
    void updateWindowSize();
    [[nodiscard]] bool needsWindowSizeSync() const;

    void handleResizing();
    void handleResized();

    void syncDpiFromWindow();
    [[nodiscard]] bool takeDpiScaleChange(float& outX, float& outY);
    void getDpiScale(float& x, float& y) const;
    void getWindowDimensions(int& width, int& height) const;

    void setWaitForIdleAfterPresent(bool enabled) { m_waitForIdleAfterPresent = enabled; }
    [[nodiscard]] bool waitForIdleAfterPresent() const { return m_waitForIdleAfterPresent; }
    [[nodiscard]] uint32_t getLastPresentedBackBufferIndex() const { return m_lastPresentedBackBufferIndex; }

    void setInformativeWindowTitle(const char* applicationName, bool includeFramerate = true, const char* extraInfo = nullptr);
    [[nodiscard]] const char* windowTitle() const;

private:
    GpuSurface(GpuDevice& device, Window* window);

    bool attachWindowed(bool createSwapChain);
    void notifyDisplayScaleChanged(float scaleX, float scaleY);

    GpuDevice& m_device;
    Window* m_window = nullptr;
    std::vector<ISurfaceObserver*> m_observers;
    std::string m_windowTitle;

    float m_dpiScaleX = 1.f;
    float m_dpiScaleY = 1.f;
    float m_prevDpiScaleX = 0.f;
    float m_prevDpiScaleY = 0.f;

    bool m_waitForIdleAfterPresent = false;
    uint32_t m_lastPresentedBackBufferIndex = 0;
};

} // namespace caustica

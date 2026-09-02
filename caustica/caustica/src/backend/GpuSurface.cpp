#include <backend/GpuSurface.h>
#include <backend/GpuDevice.h>
#include <platform/window.h>
#include <platform/glfw_window.h>
#include <rhi/utils.h>
#include <core/log.h>

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace caustica
{

GpuSurface::GpuSurface(GpuDevice& device, Window* window)
    : m_device(device)
    , m_window(window)
{
    m_device.attachSurface(this);
    if (m_window)
        m_windowTitle = m_window->getTitle();
}

GpuSurface::~GpuSurface()
{
    if (m_device.surface() == this)
        m_device.attachSurface(nullptr);
}

std::unique_ptr<GpuSurface> GpuSurface::createWindowed(GpuDevice& device, Window& window)
{
    auto surface = std::unique_ptr<GpuSurface>(new GpuSurface(device, &window));
    if (!surface->attachWindowed(true))
        return nullptr;
    return surface;
}

std::unique_ptr<GpuSurface> GpuSurface::createHeadless(GpuDevice& device)
{
    auto surface = std::unique_ptr<GpuSurface>(new GpuSurface(device, nullptr));
    if (!device.createHeadlessBackBuffers())
        return nullptr;
    surface->handleResized();
    return surface;
}

std::unique_ptr<GpuSurface> GpuSurface::adopt(GpuDevice& device, Window* window)
{
    auto surface = std::unique_ptr<GpuSurface>(new GpuSurface(device, window));
    if (window && !surface->attachWindowed(false))
        return nullptr;
    if (!window)
        surface->handleResized();
    return surface;
}

bool GpuSurface::attachWindowed(bool createSwapChain)
{
    if (!m_window)
        return false;

    if (!m_device.bindPresentTarget(*m_window))
        return false;

    if (createSwapChain && !m_device.createSwapChain())
        return false;

    m_device.m_DeviceParams.backBufferWidth = 0;
    m_device.m_DeviceParams.backBufferHeight = 0;
    updateWindowSize();

    caustica::info("GpuSurface: ready [%ux%u]", m_window->getWidth(), m_window->getHeight());
    return true;
}

void GpuSurface::addObserver(ISurfaceObserver& observer)
{
    if (std::find(m_observers.begin(), m_observers.end(), &observer) == m_observers.end())
        m_observers.push_back(&observer);
}

void GpuSurface::removeObserver(ISurfaceObserver& observer)
{
    m_observers.erase(
        std::remove(m_observers.begin(), m_observers.end(), &observer),
        m_observers.end());
}

void GpuSurface::handleResizing()
{
    m_device.beginSurfaceResize();
    for (ISurfaceObserver* observer : m_observers)
    {
        if (observer)
            observer->onSurfaceResizing();
    }
}

void GpuSurface::handleResized()
{
    m_device.endSurfaceResize();
    const BackBufferInfo info = m_device.getBackBufferInfo();
    for (ISurfaceObserver* observer : m_observers)
    {
        if (observer)
            observer->onSurfaceResized(info.width, info.height, info.sampleCount);
    }
}

void GpuSurface::notifyDisplayScaleChanged(float scaleX, float scaleY)
{
    for (ISurfaceObserver* observer : m_observers)
    {
        if (observer)
            observer->onDisplayScaleChanged(scaleX, scaleY);
    }
}

bool GpuSurface::acquireFrame()
{
    return m_device.beginFrame();
}

bool GpuSurface::presentFrame()
{
    m_lastPresentedBackBufferIndex = m_device.getCurrentBackBufferIndex();
    const bool ok = m_device.present();
    if (!m_waitForIdleAfterPresent)
        return ok;

    caustica::rhi::Device* rhiDevice = m_device.getDevice();
    if (!rhiDevice)
        return ok;

    if (!rhiDevice->waitForIdle())
    {
        caustica::error("GpuSurface: device lost while draining after present (frame %u)",
            m_lastPresentedBackBufferIndex);
        return false;
    }
    return ok;
}

bool GpuSurface::needsWindowSizeSync() const
{
    if (!m_window)
        return false;

    const int width = static_cast<int>(m_window->getWidth());
    const int height = static_cast<int>(m_window->getHeight());

    if (width == 0 || height == 0)
        return m_device.m_CanPresentSwapChain;

    if (!m_device.m_CanPresentSwapChain)
        return true;

    if (int(m_device.m_DeviceParams.backBufferWidth) != width
        || int(m_device.m_DeviceParams.backBufferHeight) != height)
        return true;

    if (m_device.m_DeviceParams.vsyncEnabled != m_device.m_RequestedVSync)
        return true;

    return false;
}

void GpuSurface::updateWindowSize()
{
    if (!m_window)
        return;

    const int width = static_cast<int>(m_window->getWidth());
    const int height = static_cast<int>(m_window->getHeight());

    if (width == 0 || height == 0)
    {
        m_device.m_CanPresentSwapChain = false;
        return;
    }

    m_device.m_CanPresentSwapChain = true;

    if (int(m_device.m_DeviceParams.backBufferWidth) != width
        || int(m_device.m_DeviceParams.backBufferHeight) != height
        || (m_device.m_DeviceParams.vsyncEnabled != m_device.m_RequestedVSync
            && m_device.getGraphicsAPI() == caustica::rhi::GraphicsAPI::VULKAN))
    {
        handleResizing();

        m_device.m_DeviceParams.backBufferWidth = uint32_t(width);
        m_device.m_DeviceParams.backBufferHeight = uint32_t(height);
        m_device.m_DeviceParams.vsyncEnabled = m_device.m_RequestedVSync;

        m_device.resizeSwapChain();
        handleResized();
    }

    m_device.m_DeviceParams.vsyncEnabled = m_device.m_RequestedVSync;
}

void GpuSurface::syncDpiFromWindow()
{
    if (!m_window)
        return;
    m_dpiScaleX = m_window->getDPIScaleX();
    m_dpiScaleY = m_window->getDPIScaleY();
}

bool GpuSurface::takeDpiScaleChange(float& outX, float& outY)
{
    if (m_prevDpiScaleX == m_dpiScaleX && m_prevDpiScaleY == m_dpiScaleY)
        return false;

    outX = m_dpiScaleX;
    outY = m_dpiScaleY;
    m_prevDpiScaleX = m_dpiScaleX;
    m_prevDpiScaleY = m_dpiScaleY;
    notifyDisplayScaleChanged(outX, outY);
    return true;
}

void GpuSurface::getDpiScale(float& x, float& y) const
{
    if (m_window)
    {
        x = m_window->getDPIScaleX();
        y = m_window->getDPIScaleY();
        return;
    }
    x = m_dpiScaleX;
    y = m_dpiScaleY;
}

void GpuSurface::getWindowDimensions(int& width, int& height) const
{
    if (m_window)
    {
        width = static_cast<int>(m_window->getWidth());
        height = static_cast<int>(m_window->getHeight());
        return;
    }

    width = int(m_device.m_DeviceParams.backBufferWidth);
    height = int(m_device.m_DeviceParams.backBufferHeight);
}

void GpuSurface::setInformativeWindowTitle(
    const char* applicationName, bool includeFramerate, const char* extraInfo)
{
    if (!m_window)
        return;

    std::stringstream ss;
    ss << applicationName;
    if (caustica::rhi::Device* rhi = m_device.getDevice())
        ss << " (" << caustica::rhi::utils::GraphicsAPIToString(rhi->getGraphicsAPI());
    else
        ss << " (";

    if (m_device.m_DeviceParams.enableDebugRuntime)
    {
        if (m_device.getGraphicsAPI() == caustica::rhi::GraphicsAPI::VULKAN)
            ss << ", VulkanValidationLayer";
        else
            ss << ", DebugRuntime";
    }

    if (m_device.m_DeviceParams.enableRhiValidationLayer)
        ss << ", RhiValidationLayer";

    ss << ")";

    const double frameTime = m_device.getAverageFrameTimeSeconds();
    if (includeFramerate && frameTime > 0)
    {
        const double fps = 1.0 / frameTime;
        const int precision = (fps <= 20.0) ? 1 : 0;
        ss << " - " << std::fixed << std::setprecision(precision) << fps << " FPS ";
    }

    if (extraInfo)
        ss << extraInfo;

    m_windowTitle = ss.str();
    m_window->setWindowTitle(m_windowTitle);
}

const char* GpuSurface::windowTitle() const
{
    return m_windowTitle.c_str();
}

} // namespace caustica

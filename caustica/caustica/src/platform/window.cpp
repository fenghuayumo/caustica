#include "platform/window.h"

namespace caustica
{

// Static factory function pointer — set by platform init (e.g. GlfwWindow::makeDefault)
Window* (*Window::s_CreateFunc)(const WindowDesc&) = nullptr;

Window* Window::create(const WindowDesc& desc)
{
    if (s_CreateFunc)
        return s_CreateFunc(desc);
    return nullptr;
}

void Window::onFocusChanged(bool focused)
{
    m_HasFocus = focused;
}

void Window::onIconifyChanged(bool iconified)
{
    m_Visible = !iconified;
}

void Window::onMove(int /*x*/, int /*y*/)
{
    // Base: no-op. GlfwWindow overrides with DPI tracking.
}

} // namespace caustica

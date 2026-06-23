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

} // namespace caustica

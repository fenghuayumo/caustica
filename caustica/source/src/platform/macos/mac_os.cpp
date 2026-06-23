#include "platform/macos/mac_os.h"

#include <mach-o/dyld.h>
#include <limits.h>

namespace caustica
{

void MacOS::init()
{
    // Register GLFW as the default window backend
}

void MacOS::run()
{
    // Application lifecycle driven by entry_point.h
}

std::string MacOS::getExecutablePath()
{
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
        return std::string(buf);
    return "";
}

} // namespace caustica

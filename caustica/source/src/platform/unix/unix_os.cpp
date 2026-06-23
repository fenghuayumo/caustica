#include "platform/unix/unix_os.h"

#include <unistd.h>
#include <limits.h>

namespace caustica
{

void UnixOS::init()
{
    // Register GLFW as the default window backend
}

void UnixOS::run()
{
    // Application lifecycle driven by entry_point.h
}

std::string UnixOS::getExecutablePath()
{
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len != -1)
    {
        buf[len] = '\0';
        return std::string(buf);
    }
    return "";
}

} // namespace caustica

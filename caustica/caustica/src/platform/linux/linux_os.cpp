#include "platform/engine/os.h"

#ifndef _WIN32

#include <dlfcn.h>
#include <limits.h>
#include <unistd.h>

namespace caustica
{

OS* OS::s_Instance = nullptr;

void OS::initialize()
{
    static UnixOS os;
    s_Instance = &os;
}

OS& OS::get()
{
    if (!s_Instance) initialize();
    return *s_Instance;
}

std::filesystem::path OS::getExecutableDirectory() const
{
    return getExecutablePath().parent_path();
}

std::filesystem::path UnixOS::getExecutablePath() const
{
    char path[PATH_MAX] = {};
    const ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0)
        return std::filesystem::current_path();
    path[n] = '\0';
    return std::filesystem::path(path);
}

void* UnixOS::loadLibrary(const std::string& path) const
{
    return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}

void UnixOS::unloadLibrary(void* handle) const
{
    if (handle)
        dlclose(handle);
}

void* UnixOS::getLibrarySymbol(void* handle, const std::string& name) const
{
    if (!handle)
        return nullptr;
    return dlsym(handle, name.c_str());
}

} // namespace caustica

#endif

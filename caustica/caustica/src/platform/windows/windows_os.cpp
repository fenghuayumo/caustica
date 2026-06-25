#include "platform/engine/os.h"

#ifdef _WIN32
#include <windows.h>

namespace caustica
{

OS* OS::s_Instance = nullptr;

void OS::initialize()
{
    static WindowsOS os;
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

// --- WindowsOS ---

WindowsOS::WindowsOS()
    : m_HInstance(GetModuleHandleA(nullptr))
{}

std::filesystem::path WindowsOS::getExecutablePath() const
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, sizeof(path));
    return std::filesystem::path(path);
}

void* WindowsOS::loadLibrary(const std::string& path) const
{
    return static_cast<void*>(LoadLibraryA(path.c_str()));
}

void WindowsOS::unloadLibrary(void* handle) const
{
    if (handle) FreeLibrary(static_cast<HMODULE>(handle));
}

void* WindowsOS::getLibrarySymbol(void* handle, const std::string& name) const
{
    if (!handle) return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name.c_str()));
}

} // namespace caustica

#endif // _WIN32

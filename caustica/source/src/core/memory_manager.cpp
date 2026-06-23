#include "core/memory_manager.h"

#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace caustica
{

MemoryManager* MemoryManager::s_Instance = nullptr;

MemoryManager::MemoryManager()
{
    s_Instance = this;
}

MemoryManager::~MemoryManager()
{
    s_Instance = nullptr;
}

SystemMemoryInfo MemoryManager::getSystemInfo()
{
    SystemMemoryInfo result = {};

#ifdef _WIN32
    MEMORYSTATUSEX status;
    status.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&status);

    result.availablePhysicalBytes = static_cast<int64_t>(status.ullAvailPhys);
    result.totalPhysicalBytes     = static_cast<int64_t>(status.ullTotalPhys);
    result.availableVirtualBytes  = static_cast<int64_t>(status.ullAvailVirtual);
    result.totalVirtualBytes      = static_cast<int64_t>(status.ullTotalVirtual);
#endif

    return result;
}

void SystemMemoryInfo::log() const
{
    auto toMB = [](int64_t bytes) -> float {
        return static_cast<float>(bytes) / (1024.0f * 1024.0f);
    };

    fprintf(stdout, "  Physical Memory : %.0f MB / %.0f MB\n",
        toMB(totalPhysicalBytes - availablePhysicalBytes), toMB(totalPhysicalBytes));
    fprintf(stdout, "  Virtual Memory  : %.0f MB / %.0f MB\n",
        toMB(totalVirtualBytes - availableVirtualBytes), toMB(totalVirtualBytes));
}

void MemoryManager::logMemoryInformation()
{
    auto info = getSystemInfo();
    info.log();
}

void MemoryManager::onShutdown()
{
    // Placeholder: verify no outstanding allocations when tracking is enabled
}

} // namespace caustica

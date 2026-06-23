/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "core/core_system.h"
#include "core/command_line.h"
#include "core/job_system.h"
#include "core/memory_manager.h"
#include "core/arena.h"
#include "platform/file_system.h"

#include <cstdio>
#include <cstdarg>
#include <ctime>

namespace caustica
{

namespace
{
    static Arena*      s_SystemArena = nullptr;
    static CommandLine s_CommandLine;

    // Minimal logging until the engine log system is wired in (Phase B).
    // These print to stdout with a timestamp prefix.
    static void coreLog(const char* level, const char* fmt...)
    {
        time_t now = time(nullptr);
        char timeBuf[32];
        strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", localtime(&now));

        fprintf(stdout, "[%s] [caustica::core] ", timeBuf);

        va_list args;
        va_start(args, fmt);
        vfprintf(stdout, fmt, args);
        va_end(args);
        fprintf(stdout, "\n");
    }
}

bool coresystem::init(int argc, char** argv)
{
    coreLog("INFO", "CoreSystem initializing...");

    // 1. Create system memory arena (2 MB for early allocations)
    s_SystemArena = ArenaAlloc(Megabytes(2));
    coreLog("INFO", "System arena allocated (2 MB)");

    // 2. Parse command line
    s_CommandLine.parse(argc, const_cast<const char**>(argv));

    if (s_CommandLine.hasOption("help"))
    {
        fprintf(stdout, "caustica — Real-time Path Tracing Renderer\n");
        fprintf(stdout, "Options:\n");
        fprintf(stdout, "  --help          Show this help\n");
        fprintf(stdout, "  --noWindow      Run headless\n");
        fprintf(stdout, "  --width=N       Set window width\n");
        fprintf(stdout, "  --height=N      Set window height\n");
        fprintf(stdout, "  --scene=NAME    Scene to load\n");
        fprintf(stdout, "  --backend=API   Graphics API (vk/dx12)\n");
    }

    // 3. Initialize job system (reserve 2 threads: main + render)
    core::JobSystem::init(2);
    coreLog("INFO", "JobSystem initialized with %u workers", core::JobSystem::numWorkers());

    // 4. Initialize file system
    FileSystem::get().init();
    coreLog("INFO", "FileSystem initialized");

    // 5. Log system memory info
    auto sysInfo = MemoryManager::getSystemInfo();
    coreLog("INFO", "System Memory:");
    sysInfo.log();

    return true;
}

void coresystem::shutdown()
{
    coreLog("INFO", "CoreSystem shutting down...");

    // Shut down in reverse order
    FileSystem::release();
    core::JobSystem::shutdown();

    MemoryManager::logMemoryInformation();

    if (s_SystemArena)
    {
        ArenaRelease(s_SystemArena);
        s_SystemArena = nullptr;
    }

    coreLog("INFO", "CoreSystem shutdown complete");
}

CommandLine* coresystem::getCommandLine()
{
    return &s_CommandLine;
}

} // namespace caustica

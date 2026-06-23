/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#pragma once

#include <cstdint>
#include <string>

namespace caustica
{

// System memory information (queried at startup)
struct SystemMemoryInfo
{
    int64_t availablePhysicalBytes = 0;
    int64_t totalPhysicalBytes     = 0;
    int64_t availableVirtualBytes  = 0;
    int64_t totalVirtualBytes      = 0;

    void log() const;
};

// Memory manager — tracks allocation stats and provides system info.
// Wraps the existing MemoryManager found in the engine layer.
class MemoryManager
{
public:
    static MemoryManager* get() { return s_Instance; }

    MemoryManager();
    ~MemoryManager();

    // Query system memory info (platform-specific implementation)
    static SystemMemoryInfo getSystemInfo();

    // Log current memory usage stats
    static void logMemoryInformation();

    // Called at shutdown to verify no leaks
    static void onShutdown();

private:
    static MemoryManager* s_Instance;
};

} // namespace caustica

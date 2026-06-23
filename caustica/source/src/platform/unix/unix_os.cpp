/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

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

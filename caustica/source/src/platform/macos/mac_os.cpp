/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

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

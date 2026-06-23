/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

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

/*
Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#ifdef _WIN32
// Screenshot paths arrive as UTF-8 from Python and the UI. Use wide Windows
// file APIs instead of interpreting their bytes through the ANSI code page.
#define STBIW_WINDOWS_UTF8
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

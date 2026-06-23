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

#include "platform/os.h"

namespace caustica
{

class WindowsOS : public OS
{
public:
    WindowsOS()  = default;
    ~WindowsOS() = default;

    void init() override;
    void run() override;

    std::string getExecutablePath() override;

    void openFileLocation(const std::filesystem::path& path) override;
    void openFileExternal(const std::filesystem::path& path) override;
    void openURL(const std::string& url) override;

    void setTitleBarColour(const std::array<float,4>& colour, bool dark = true) override;
};

} // namespace caustica

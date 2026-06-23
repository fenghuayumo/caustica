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

#pragma once

#include "platform/os.h"

namespace caustica
{

class MacOS : public OS
{
public:
    MacOS()  = default;
    ~MacOS() = default;

    void init() override;
    void run() override;

    std::string getExecutablePath() override;
};

} // namespace caustica

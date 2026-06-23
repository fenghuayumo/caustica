#pragma once

#include "platform/os.h"

namespace caustica
{

class UnixOS : public OS
{
public:
    UnixOS()  = default;
    ~UnixOS() = default;

    void init() override;
    void run() override;

    std::string getExecutablePath() override;
};

} // namespace caustica

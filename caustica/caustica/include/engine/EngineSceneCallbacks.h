#pragma once

#include <functional>

namespace caustica
{

struct EngineSceneCallbacks
{
    std::function<void()> OnSceneLoaded;
    std::function<void()> OnSceneUnloading;
};

} // namespace caustica

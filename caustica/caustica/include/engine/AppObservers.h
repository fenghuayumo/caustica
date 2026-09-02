#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace caustica
{

class Event;

// GLFW delivers stack-allocated events during poll, so observers are invoked
// immediately. queueEvent() uses the same dispatch path from ProcessEventQueue.
struct EventObservers
{
    std::vector<std::function<void(Event&)>> handlers;
};

} // namespace caustica

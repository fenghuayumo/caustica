#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace caustica
{

class Event;

// Window callbacks enqueue unique_ptr<Event>. App drains them in First
// (ProcessEventQueue): InputState first, then EventObservers.
struct EventObservers
{
    std::vector<std::function<void(Event&)>> handlers;
};

} // namespace caustica

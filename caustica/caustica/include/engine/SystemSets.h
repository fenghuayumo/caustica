#pragma once

#include <engine/SystemLabel.h>

namespace caustica::system_set
{

// Default Bevy-style groupings. Systems declare membership via
// AppSystemOrdering::inSet<...>(); AppSchedules applies set-level edges.
struct Simulation
{
    static constexpr const char* name = "SystemSet.Simulation";
};

struct TransformPropagate
{
    static constexpr const char* name = "SystemSet.TransformPropagate";
};

struct Extract
{
    static constexpr const char* name = "SystemSet.Extract";
};

} // namespace caustica::system_set

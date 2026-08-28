#pragma once

#include <cstdint>

namespace caustica
{

// Frame timing as an App resource, refreshed once per frame before the
// simulation schedules run.
//
// Systems should take `Res<Time>` rather than `SystemContext&` for timing.
// SystemContext hands out `App&`, which reaches the whole engine, so the
// scheduler has to run those systems exclusively (ADR 0003). Reading the clock
// is the single most common reason a gameplay system needed the context, so
// having it here is what lets ordinary per-frame systems run in parallel.
struct Time
{
    // Seconds since the previous frame.
    float deltaSeconds = 0.f;
    // Seconds since the first frame.
    double elapsedSeconds = 0.0;
    uint64_t frameCount = 0;
};

} // namespace caustica

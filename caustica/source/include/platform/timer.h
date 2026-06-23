#pragma once

#include <chrono>

namespace caustica
{

// Platform-independent high-resolution timer.
// Wraps std::chrono::high_resolution_clock.
class Timer
{
public:
    Timer();

    // Reset the start point
    void reset();

    // Elapsed time since construction or last reset
    double getElapsedSeconds() const;
    double getElapsedMilliseconds() const;

    // Convenience: start + stop pattern
    void start();
    void stop();
    double seconds() const;       // between last start/stop
    double milliseconds() const;  // between last start/stop

private:
    using Clock     = std::chrono::high_resolution_clock;
    using TimePoint = std::chrono::time_point<Clock>;

    TimePoint m_StartTime;
    TimePoint m_StopTime;
    bool      m_Running = false;
};

} // namespace caustica

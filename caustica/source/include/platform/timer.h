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

/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto. Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "platform/timer.h"

namespace caustica
{

Timer::Timer()
    : m_StartTime(Clock::now())
{}

void Timer::reset()
{
    m_StartTime = Clock::now();
    m_Running   = true;
}

double Timer::getElapsedSeconds() const
{
    auto now    = Clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_StartTime);
    return elapsed.count() * 1e-9;
}

double Timer::getElapsedMilliseconds() const
{
    return getElapsedSeconds() * 1e3;
}

void Timer::start()
{
    m_StartTime = Clock::now();
    m_Running   = true;
}

void Timer::stop()
{
    m_StopTime = Clock::now();
    m_Running  = false;
}

double Timer::seconds() const
{
    auto end   = m_Running ? Clock::now() : m_StopTime;
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - m_StartTime);
    return elapsed.count() * 1e-9;
}

double Timer::milliseconds() const
{
    return seconds() * 1e3;
}

} // namespace caustica

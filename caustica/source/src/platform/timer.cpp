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

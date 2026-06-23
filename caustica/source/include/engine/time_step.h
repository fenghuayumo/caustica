#pragma once

#include <cstdint>

namespace caustica
{

// Frame time tracking. Updated each frame in the main loop.
class TimeStep
{
public:
    TimeStep() = default;

    void update(double elapsedSeconds);

    double getSeconds()      const { return m_Seconds; }
    double getMilliseconds() const { return m_Milliseconds; }
    float  getSecondsF()     const { return static_cast<float>(m_Seconds); }
    float  getMillisecondsF() const { return static_cast<float>(m_Milliseconds); }

private:
    double m_Seconds      = 0.0;
    double m_Milliseconds = 0.0;
    double m_LastTime     = 0.0;
};

} // namespace caustica

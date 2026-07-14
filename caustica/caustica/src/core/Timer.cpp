#include <core/Timer.h>

#include <algorithm>
#include <thread>

namespace caustica
{

void FPSLimiter::framerateLimit(int fpsTarget)
{
    auto nowTimestamp = std::chrono::high_resolution_clock::now();
    double deltaTime = std::chrono::duration<double>(nowTimestamp - m_lastTimestamp).count();
    double targetDeltaTime = 1.0 / static_cast<double>(fpsTarget);
    double diffFromTarget = targetDeltaTime - deltaTime + m_prevError;
    if (diffFromTarget > 0.0)
    {
        size_t sleepInMs = std::min(1000, static_cast<int>(diffFromTarget * 1000));
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepInMs));
    }

    auto prevTime = m_lastTimestamp;
    m_lastTimestamp = std::chrono::high_resolution_clock::now();
    double deltaError = targetDeltaTime -
        std::chrono::duration<double>(m_lastTimestamp - prevTime).count();
    m_prevError = deltaError * 0.9 + m_prevError * 0.1; // dampen spring-like effect

    if (m_prevError > targetDeltaTime)
        m_prevError = targetDeltaTime;
    if (m_prevError < -targetDeltaTime)
        m_prevError = -targetDeltaTime;

    m_lastTimestamp += std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(m_prevError));
}

} // namespace caustica

#pragma once
#include <chrono>

namespace caustica
{
    class HiResTimer
    {
    public:

        std::chrono::time_point<std::chrono::high_resolution_clock> startTime;
        std::chrono::time_point<std::chrono::high_resolution_clock> stopTime;

        HiResTimer()
        {
        }

        void start()
        {
            startTime = std::chrono::high_resolution_clock::now();
        }

        void stop()
        {
            stopTime = std::chrono::high_resolution_clock::now();
        }

        double Seconds()
        {
            auto duration = stopTime - startTime;
            return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() * 1e-9;
        }

        double Milliseconds()
        {
            return Seconds() * 1e3;
        }
    };

// Simple FPS limiter — sleeps to keep frame rate near target.
// Not intended for precise frame pacing, but useful for avoiding GPU overheating.
class FPSLimiter
{
public:
    void framerateLimit(int fpsTarget);

private:
    std::chrono::high_resolution_clock::time_point m_lastTimestamp =
        std::chrono::high_resolution_clock::now();
    double m_prevError = 0.0;
};
}
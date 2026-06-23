#pragma once

#include "engine/time_step.h"
#include <cstdint>
#include <memory>

namespace caustica
{

// Engine singleton — holds global timing, statistics, and frame rate control.
// Created once during Application::init(), destroyed during Application::quit().
class Engine
{
public:
    Engine();
    ~Engine();

    // Singleton access
    static Engine& get();
    static void    release();

    // Frame rate
    float targetFrameRate() const          { return m_TargetFrameRate; }
    void  setTargetFrameRate(float fps)    { m_TargetFrameRate = fps; }

    // Time since last frame
    static TimeStep& timeStep()            { return *get().m_TimeStep; }

    // Runtime statistics
    struct Stats
    {
        uint32_t UpdatesPerSecond  = 0;
        uint32_t FramesPerSecond   = 0;
        uint32_t NumDrawCalls      = 0;
        uint32_t NumRenderedObjects = 0;
        double   FrameTimeMs       = 0.0;
        float    UsedGPUMemoryMB   = 0.0f;
        float    UsedRAM_MB        = 0.0f;
    };

    Stats& statistics() { return m_Stats; }
    void   resetStats();

private:
    float     m_TargetFrameRate = 1000.0f / 60.0f;  // ~16.67 ms default
    TimeStep* m_TimeStep = nullptr;
    Stats     m_Stats;

    static Engine* s_Instance;
};

} // namespace caustica

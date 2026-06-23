/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*/

#include "engine/engine.h"

namespace caustica
{

Engine* Engine::s_Instance = nullptr;

Engine::Engine()
    : m_TargetFrameRate(1000.0f / 60.0f)
{
    s_Instance = this;
    m_TimeStep = new TimeStep();
}

Engine::~Engine()
{
    delete m_TimeStep;
    s_Instance = nullptr;
}

Engine& Engine::get()
{
    if (!s_Instance)
    {
        // Create on first access if not explicitly created
        static Engine defaultInstance;
    }
    return *s_Instance;
}

void Engine::release()
{
    if (s_Instance)
    {
        delete s_Instance;
        s_Instance = nullptr;
    }
}

void Engine::resetStats()
{
    m_Stats.NumDrawCalls       = 0;
    m_Stats.NumRenderedObjects  = 0;
    m_Stats.FrameTimeMs         = 0.0;
    m_Stats.UsedGPUMemoryMB     = 0.0f;
    m_Stats.UsedRAM_MB          = 0.0f;
}

} // namespace caustica

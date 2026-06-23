/*
* Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
*/

#include "engine/time_step.h"

namespace caustica
{

void TimeStep::update(double elapsedSeconds)
{
    m_Seconds      = elapsedSeconds;
    m_Milliseconds = elapsedSeconds * 1000.0;
    m_LastTime    += elapsedSeconds;
}

} // namespace caustica

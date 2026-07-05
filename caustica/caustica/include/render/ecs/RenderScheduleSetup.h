#pragma once

#include <ecs/Schedule.h>

namespace caustica::render
{

class FramePassRegistry;
class WorldRenderer;

void buildDefaultRenderSchedule(
    ecs::Schedule& schedule,
    WorldRenderer& renderer,
    FramePassRegistry* framePassRegistry = nullptr);

} // namespace caustica::render

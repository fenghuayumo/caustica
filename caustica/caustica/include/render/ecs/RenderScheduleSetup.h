#pragma once

#include <ecs/Schedule.h>

namespace caustica::render
{

class WorldRenderer;

void buildDefaultRenderSchedule(ecs::Schedule& schedule, WorldRenderer& renderer);

} // namespace caustica::render

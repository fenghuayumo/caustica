#pragma once

#include <render/worldRenderer/PathTracingFrameContext.h>
#include <render/graph/GraphBuilder.h>

namespace caustica::render
{

// Per-frame state for the render-thread schedule. Wraps PathTracingFrameContext
// while GraphBuilder ownership stays on WorldRenderer.
struct RenderFrameContext
{
    PathTracingFrameContext frame{};

    rg::GraphBuilder* graph = nullptr;

    bool graphBuilt = false;
    bool commandListWasClosed = false;
};

struct RenderScheduleContext
{
    float deltaTimeSeconds = 0.0f;
    uint32_t frameIndex = 0;
};

} // namespace caustica::render

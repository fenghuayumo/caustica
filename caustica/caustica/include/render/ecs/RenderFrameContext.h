#pragma once

#include <render/worldRenderer/PathTracingFrameContext.h>
#include <render/graph/GraphBuilder.h>
#include <scene/View.h>

namespace caustica::render
{

// Per-frame state for the render-thread schedule. Wraps PathTracingFrameContext
// while GraphBuilder ownership stays on WorldRenderer.
struct RenderFrameContext
{
    PathTracingFrameContext frame{};

    rg::GraphBuilder* graph = nullptr;

    // Post-process graph passes capture ICompositeView by reference; keep storage
    // alive until executeFrameRenderGraph() finishes.
    caustica::PlanarView postProcessCompositeView;

    bool graphBuilt = false;
    bool commandListWasClosed = false;
};

struct RenderScheduleContext
{
    float deltaTimeSeconds = 0.0f;
    uint32_t frameIndex = 0;
};

} // namespace caustica::render

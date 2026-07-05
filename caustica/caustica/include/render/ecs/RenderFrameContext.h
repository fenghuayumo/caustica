#pragma once

#include <render/worldRenderer/PathTracingFrameContext.h>
#include <render/graph/GraphBuilder.h>
#include <scene/SceneRenderData.h>
#include <scene/View.h>

#include <math/math.h>

namespace caustica::render
{

struct ExtractedFrameView
{
    dm::uint2 displaySize{};
    dm::uint2 renderSize{};
    float displayAspectRatio = 1.0f;
    caustica::PlanarView postProcessView;
};

// Single per-frame render-thread context. Passes and pipeline plugins read only this.
struct RenderFrameContext
{
    PathTracingFrameContext frame{};

    rg::GraphBuilder* graph = nullptr;
    bool graphBuilt = false;
    bool commandListWasClosed = false;

    const scene::SceneRenderData* scene = nullptr;
    bool sceneStructureChanged = false;
    bool sceneTransformsChanged = false;

    ExtractedFrameView view{};
};

} // namespace caustica::render

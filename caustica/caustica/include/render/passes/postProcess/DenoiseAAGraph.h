#pragma once

#include <render/graph/GraphBuilder.h>
#include <render/core/PathTracerSettings.h>
#include <render/FramePassRegistry.h>

class RenderTargets;

namespace caustica::render
{
struct PathTracingFrameContext;
}

namespace caustica::rg
{

struct DenoiseAAGraphParams
{
    GraphBuilder& graph;
    RenderTargets* renderTargets = nullptr;
    PathTracerSettings* settings = nullptr;
    render::FramePassRegistry* framePassRegistry = nullptr;
    render::PathTracingFrameContext* frameContext = nullptr;
};

// Registers denoise/AA graph passes. Currently covers the realtime no-AA copy path;
// TAA/NRD integration will extend this builder over time.
void buildDenoiseAndAAGraph(const DenoiseAAGraphParams& params);

} // namespace caustica::rg

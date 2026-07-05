#pragma once

#include <render/graph/GraphBuilder.h>
#include <render/core/PathTracerSettings.h>
#include <shaders/SampleConstantBuffer.h>

class RenderTargets;

namespace caustica::render
{
class FramePassRegistry;
class PathTracingFrameContext;
class WorldRenderer;
}

namespace caustica::rg
{

struct PathTraceGraphParams
{
    GraphBuilder& graph;
    render::WorldRenderer* worldRenderer = nullptr;
    RenderTargets* renderTargets = nullptr;
    PathTracerSettings* settings = nullptr;
    render::PathTracingFrameContext* frameContext = nullptr;
    render::FramePassRegistry* framePassRegistry = nullptr;
    nvrhi::IFramebuffer* framebuffer = nullptr;
    const SampleConstants* sampleConstants = nullptr;
    bool hasScene = true;
};

void buildPathTraceGraph(const PathTraceGraphParams& params);

} // namespace caustica::rg

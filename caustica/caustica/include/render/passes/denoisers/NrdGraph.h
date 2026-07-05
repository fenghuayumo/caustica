#pragma once

#include <render/graph/GraphBuilder.h>
#include <render/core/PathTracerSettings.h>

class RenderTargets;

namespace caustica::render
{
class WorldRenderer;
}

namespace caustica::rg
{

struct NrdGraphParams
{
    GraphBuilder& graph;
    render::WorldRenderer* worldRenderer = nullptr;
    RenderTargets* renderTargets = nullptr;
    PathTracerSettings* settings = nullptr;
    nvrhi::IFramebuffer* framebuffer = nullptr;
    bool hasScene = true;
};

void buildNrdGraph(const NrdGraphParams& params);

} // namespace caustica::rg

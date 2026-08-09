#pragma once

namespace caustica::render
{

struct FrameGraphContext;
class RenderFrameContext;
class WorldRenderer;

class IRenderPipelinePlugin
{
public:
    virtual ~IRenderPipelinePlugin() = default;

    [[nodiscard]] virtual const char* name() const = 0;

    virtual void onPrepareFrame(WorldRenderer& renderer, RenderFrameContext& ctx) {}
    virtual void buildGraph(FrameGraphContext& ctx) {}
    virtual void onFinalizeFrame(WorldRenderer& renderer, RenderFrameContext& ctx) {}
};

} // namespace caustica::render

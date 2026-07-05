#pragma once

#include <render/pipeline/IRenderPipelinePlugin.h>

namespace caustica::render
{

class PathTracingPipelinePlugin : public IRenderPipelinePlugin
{
public:
    [[nodiscard]] const char* name() const override { return "PathTracing"; }

    void onPrepareFrame(WorldRenderer& renderer, RenderFrameContext& ctx) override;
    void registerGraphPasses(RenderGraphRegistry& registry, WorldRenderer& renderer, RenderFrameContext& ctx) override;
    void onFinalizeFrame(WorldRenderer& renderer, RenderFrameContext& ctx) override;
};

} // namespace caustica::render

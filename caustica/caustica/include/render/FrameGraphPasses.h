#pragma once

#include <render/graph/GpuTypes.h>

namespace caustica::rg
{
class GraphBuilder;
}

namespace caustica::render
{

struct FrameGraphContext;

// Public image identity for this frame (version 0). Producers and consumers
// pass these handles through; read/write resolve them to the latest version.
struct FrameSlots
{
    rg::TextureHandle outputColor;
    rg::TextureHandle hdrColor;
    rg::TextureHandle ldrColor;
    rg::TextureHandle depth;
};

void seedFrameSlots(FrameSlots& slots, FrameGraphContext ctx);

void registerDefaultFrameGraphPasses(FrameGraphContext ctx);

rg::PassHandle registerClearFrameTargetsPass(FrameGraphContext ctx, FrameSlots& slots);
rg::PassHandle registerUploadFrameConstantsPass(FrameGraphContext ctx);
rg::PassHandle registerLightingGraphPasses(FrameGraphContext ctx);
rg::PassHandle registerRtxdiBeginFramePass(FrameGraphContext ctx);
rg::PassHandle registerPathTracePrePass(FrameGraphContext ctx);
rg::PassHandle registerVBufferExportPass(FrameGraphContext ctx);
rg::PassHandle registerPathTraceLightingEndPass(FrameGraphContext ctx);
rg::PassHandle registerGaussianSplatAccelBuildPass(FrameGraphContext ctx);
rg::PassHandle registerMainPathTracePass(FrameGraphContext ctx);
rg::PassHandle registerRtxdiExecutePass(FrameGraphContext ctx);
rg::PassHandle registerDenoiserPreparePass(FrameGraphContext ctx);
rg::PassHandle registerNrdPass(FrameGraphContext ctx);
rg::PassHandle registerGaussianSplatPreAAPass(FrameGraphContext ctx, FrameSlots& slots);
rg::PassHandle registerDenoiseAAPass(FrameGraphContext ctx, FrameSlots& slots);
rg::PassHandle registerGaussianSplatCompositePass(FrameGraphContext ctx, FrameSlots& slots);
void registerPostProcessGraphPasses(FrameGraphContext ctx, FrameSlots& slots);
rg::PassHandle registerCompositeGraphPasses(FrameGraphContext ctx, FrameSlots& slots);
rg::PassHandle registerDebugOverlayGraphPasses(FrameGraphContext ctx, FrameSlots& slots);

} // namespace caustica::render

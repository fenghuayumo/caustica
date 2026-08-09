#pragma once

#include <render/graph/GpuTypes.h>

namespace caustica::render
{

struct FrameGraphContext;

void registerDefaultFrameGraphPasses(FrameGraphContext ctx);

[[nodiscard]] rg::PassHandle registerClearFrameTargetsPass(FrameGraphContext ctx);
// Writes FrameConstants after Clear (graph-owned; not pre-graph CL).
[[nodiscard]] rg::PassHandle registerUploadFrameConstantsPass(
    FrameGraphContext ctx, rg::PassHandle after);
// Registers EnvMapUpdate → LightSamplingUpdateBegin → UploadSubInstanceData.
[[nodiscard]] rg::PassHandle registerLightingGraphPasses(
    FrameGraphContext ctx, rg::PassHandle after);
[[nodiscard]] rg::PassHandle registerRtxdiBeginFramePass(
    FrameGraphContext ctx, rg::PassHandle after);
[[nodiscard]] rg::PassHandle registerPathTracePrePass(
    FrameGraphContext ctx, rg::PassHandle after);
[[nodiscard]] rg::PassHandle registerVBufferExportPass(
    FrameGraphContext ctx, rg::PassHandle after);
[[nodiscard]] rg::PassHandle registerPathTraceLightingEndPass(
    FrameGraphContext ctx, rg::PassHandle after);
[[nodiscard]] rg::PassHandle registerGaussianSplatAccelBuildPass(
    FrameGraphContext ctx, rg::PassHandle after);
[[nodiscard]] rg::PassHandle registerMainPathTracePass(
    FrameGraphContext ctx, rg::PassHandle after);
[[nodiscard]] rg::PassHandle registerRtxdiExecutePass(
    FrameGraphContext ctx, rg::PassHandle after);
[[nodiscard]] rg::PassHandle registerDenoiserPreparePass(
    FrameGraphContext ctx, rg::PassHandle after);
[[nodiscard]] rg::PassHandle registerNrdPass(
    FrameGraphContext ctx, rg::PassHandle guidesReady);
[[nodiscard]] rg::PassHandle registerGaussianSplatPreAAPass(FrameGraphContext ctx);
[[nodiscard]] rg::PassHandle registerDenoiseAAPass(FrameGraphContext ctx);
[[nodiscard]] rg::PassHandle registerGaussianSplatCompositePass(FrameGraphContext ctx);
void registerPostProcessGraphPasses(FrameGraphContext ctx);
[[nodiscard]] rg::PassHandle registerCompositeGraphPasses(FrameGraphContext ctx);
[[nodiscard]] rg::PassHandle registerDebugOverlayGraphPasses(
    FrameGraphContext ctx, rg::PassHandle after);

} // namespace caustica::render

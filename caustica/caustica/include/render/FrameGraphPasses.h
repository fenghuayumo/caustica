#pragma once

namespace caustica::render
{

struct FrameGraphContext;

void registerDefaultFrameGraphPasses(FrameGraphContext ctx);

void registerClearFrameTargetsPass(FrameGraphContext ctx);
// Writes FrameConstants after Clear (graph-owned; not pre-graph CL).
void registerUploadFrameConstantsPass(FrameGraphContext ctx);
// Registers EnvMapUpdate → LightSamplingUpdateBegin → UploadSubInstanceData.
void registerLightingGraphPasses(FrameGraphContext ctx);
void registerRtxdiBeginFramePass(FrameGraphContext ctx);
void registerPathTracePrePass(FrameGraphContext ctx);
void registerVBufferExportPass(FrameGraphContext ctx);
void registerPathTraceLightingEndPass(FrameGraphContext ctx);
void registerGaussianSplatAccelBuildPass(FrameGraphContext ctx);
void registerMainPathTracePass(FrameGraphContext ctx);
void registerRtxdiExecutePass(FrameGraphContext ctx);
void registerDenoiserPreparePass(FrameGraphContext ctx);
void registerNrdPass(FrameGraphContext ctx);
void registerGaussianSplatPreAAPass(FrameGraphContext ctx);
void registerDenoiseAAPass(FrameGraphContext ctx);
void registerGaussianSplatCompositePass(FrameGraphContext ctx);
void registerPostProcessGraphPasses(FrameGraphContext ctx);
void registerCompositeGraphPasses(FrameGraphContext ctx);
void registerDebugOverlayGraphPasses(FrameGraphContext ctx);

} // namespace caustica::render

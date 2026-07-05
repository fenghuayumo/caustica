#pragma once

namespace caustica::render
{

struct RenderFeatureContext;

void registerDefaultGraphFeatures(RenderFeatureContext ctx);

void registerPathTracePrePassFeature(RenderFeatureContext ctx);
void registerVBufferExportFeature(RenderFeatureContext ctx);
void registerPathTraceLightingEndFeature(RenderFeatureContext ctx);
void registerMainPathTraceFeature(RenderFeatureContext ctx);
void registerRtxdiBeginFrameFeature(RenderFeatureContext ctx);
void registerRtxdiExecuteFeature(RenderFeatureContext ctx);
void registerDenoiserPrepareFeature(RenderFeatureContext ctx);

void registerNrdFeature(RenderFeatureContext ctx);
void registerDenoiseAAFeature(RenderFeatureContext ctx);
void registerPostProcessFeature(RenderFeatureContext ctx);
void registerCompositeFeature(RenderFeatureContext ctx);

} // namespace caustica::render

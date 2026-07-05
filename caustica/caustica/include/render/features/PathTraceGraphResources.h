#pragma once

#include <render/graph/GraphBuilder.h>

class RenderTargets;

namespace caustica::render
{

struct RenderFeatureContext;

struct PathTraceGraphTargets
{
    rg::TextureHandle outputColor;
    rg::TextureHandle processedOutputColor;
    rg::TextureHandle depth;
    rg::TextureHandle motionVectors;
    rg::TextureHandle throughput;
    rg::TextureHandle specularHitT;
    rg::TextureHandle scratchFloat1;
    rg::TextureHandle stableRadiance;
    rg::TextureHandle stablePlanesHeader;
    rg::BufferHandle stablePlanesBuffer;
    rg::TextureHandle denoiserViewspaceZ;
    rg::TextureHandle denoiserMotionVectors;
    rg::TextureHandle denoiserNormalRoughness;
    rg::TextureHandle denoiserDiffRadianceHitDist;
    rg::TextureHandle denoiserSpecRadianceHitDist;
    rg::TextureHandle denoiserDisocclusionThresholdMix;
    rg::TextureHandle denoiserAvgLayerRadianceHalfRes;
    rg::TextureHandle baseColor;
    rg::TextureHandle specNormal;
    rg::TextureHandle roughnessMetal;
    rg::TextureHandle materialInfo;
    rg::BufferHandle surfaceDataBuffer;
    rg::TextureHandle secondarySurfacePositionNormal;
    rg::TextureHandle secondarySurfaceRadiance;
};

PathTraceGraphTargets importPathTraceGraphTargets(rg::GraphBuilder& graph, RenderTargets& targets);

void extractPathTraceGraphOutputs(rg::GraphBuilder& graph, const PathTraceGraphTargets& handles);

void declarePathTraceOutputWrites(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);
void declarePathTraceLightingEndAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);

void declarePathTracePrePassAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);
void declareVBufferExportAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);
void declareMainPathTraceAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);
void declareRtxdiBeginFrameAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);
void declareRtxdiExecuteAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);
void declareDenoiserPrepareAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);
void declareStablePlanesDebugVizAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);

} // namespace caustica::render

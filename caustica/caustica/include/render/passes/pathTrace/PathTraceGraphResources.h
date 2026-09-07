#pragma once

#include <render/core/PathTracerSettings.h>
#include <render/graph/GraphBuilder.h>

class LightSamplingCache;
class RenderTargets;

namespace caustica::render
{

struct FrameGraphContext;

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
    rg::TextureHandle sensorNormalDepth;
    rg::TextureHandle sensorIds;
    rg::TextureHandle sensorMaterial;
    rg::TextureHandle sensorSpecular;
};

struct PathTraceLightingEndTargets
{
    rg::TextureHandle depth;
    rg::TextureHandle motionVectors;
    rg::TextureHandle feedbackTotalWeight;
    rg::TextureHandle feedbackCandidates;
    rg::TextureHandle feedbackTotalWeightScratch;
    rg::TextureHandle feedbackCandidatesScratch;
    rg::TextureHandle feedbackTotalWeightBlended;
    rg::TextureHandle feedbackCandidatesBlended;
    rg::TextureHandle historyDepth;
    rg::BufferHandle localSamplingBuffer;
    rg::BufferHandle subInstanceData;
};

PathTraceGraphTargets importPathTraceGraphTargets(rg::GraphBuilder& graph, RenderTargets& targets);

PathTraceLightingEndTargets importPathTraceLightingEndTargets(
    rg::GraphBuilder& graph,
    RenderTargets& targets,
    LightSamplingCache* lightSampling,
    caustica::rhi::Buffer* subInstanceDataBuffer);

struct PathTraceScheduleInputs
{
    rg::TextureHandle envCube;
    rg::BufferHandle lightBuffer;
    rg::BufferHandle subInstance;
    rg::BufferHandle constants;
    rg::TextureHandle feedbackTotalWeight;
    rg::TextureHandle feedbackCandidates;
    rg::AccelStructHandle sceneAS;
    rg::AccelStructHandle gaussianAS;
};

inline constexpr const char* kScratchFloat1Name = "scratchFloat1";
inline constexpr const char* kAvgLayerRadianceName = "denoiserAvgLayerRadianceHalfRes";
inline constexpr const char* kLdrColorScratchName = "ldrColorScratch";

PathTraceScheduleInputs importPathTraceScheduleInputs(const FrameGraphContext& ctx);

void declarePathTraceScheduleReads(rg::PassBuilder& setup, const PathTraceScheduleInputs& inputs);

void declarePathTraceOutputWrites(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);
void declarePathTraceLightingEndAccess(rg::PassBuilder& setup, const PathTraceLightingEndTargets& handles);

void declarePathTracePrePassAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);
void declareVBufferExportAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);
void declareMainPathTraceAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);
void declareDenoiserPrepareAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);
void declareStablePlanesDebugVizAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles);

[[nodiscard]] bool needsPathTraceLightingEndPass(const PathTracerSettings& settings);

void validateReferencePathTraceGraph(const rg::GraphBuilder& graph, const PathTracerSettings& settings);

} // namespace caustica::render

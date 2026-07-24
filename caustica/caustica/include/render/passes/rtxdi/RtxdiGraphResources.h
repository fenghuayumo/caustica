#pragma once

#include <render/graph/GraphBuilder.h>

class RtxdiPass;
struct PathTracerSettings;

namespace caustica::render
{

struct PathTraceGraphTargets;

struct RtxdiGraphResources
{
    rg::BufferHandle risBuffer;
    rg::BufferHandle lightDataBuffer;
    rg::BufferHandle risLightDataBuffer;
    rg::BufferHandle lightReservoirBuffer;
    rg::BufferHandle giReservoirBuffer;
    rg::BufferHandle ptReservoirBuffer;
    rg::TextureHandle localLightPdf;
};

[[nodiscard]] bool tryImportRtxdiGraphResources(
    rg::GraphBuilder& graph,
    RtxdiPass* rtxdiPass,
    RtxdiGraphResources& outResources);

void declareRtxdiBeginFrameAccess(
    rg::PassBuilder& setup,
    const RtxdiGraphResources& rtxdiResources,
    const PathTraceGraphTargets& pathTraceTargets);

void declareRtxdiPrepareLightsAccess(
    rg::PassBuilder& setup,
    const RtxdiGraphResources& rtxdiResources);

void declareRtxdiGeneratePdfMipsAccess(
    rg::PassBuilder& setup,
    const RtxdiGraphResources& rtxdiResources);

void declareRtxdiPresampleAccess(
    rg::PassBuilder& setup,
    const RtxdiGraphResources& rtxdiResources);

void declareRtxdiExecuteAccess(
    rg::PassBuilder& setup,
    const RtxdiGraphResources& rtxdiResources,
    const PathTraceGraphTargets& pathTraceTargets,
    const PathTracerSettings& settings);

} // namespace caustica::render

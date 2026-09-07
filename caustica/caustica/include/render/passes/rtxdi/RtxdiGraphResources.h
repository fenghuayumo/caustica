#pragma once

#include <render/graph/GraphBuilder.h>

class RtxdiPass;

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
    // Binding-set inputs sampled by the RTXDI shaders but invisible to the
    // descriptor table. Declaring them orders UploadSubInstanceData (Copy) and
    // EnvMapUpdate (Compute) before the Compute-queue begin stages and restores
    // the UAV -> SRV transitions.
    rg::BufferHandle subInstanceData;
    rg::TextureHandle envCube;
    rg::TextureHandle radianceImportance;
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
    bool useGI,
    bool usePT);

} // namespace caustica::render

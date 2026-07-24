#include <render/passes/pathTrace/PathTraceGraphResources.h>

#include <render/core/RenderTargets.h>
#include <render/core/PathTracerSettings.h>
#include <render/passes/gaussian/GaussianSplatGraph.h>
#include <render/pipeline/FrameGraphPassNames.h>

#include <cassert>

namespace caustica::render
{

PathTraceGraphTargets importPathTraceGraphTargets(rg::GraphBuilder& graph, RenderTargets& targets)
{
    return PathTraceGraphTargets{
        .outputColor = graph.importTexture(targets.outputColor, rg::TextureAccess::UnorderedAccess),
        .processedOutputColor = graph.importTexture(targets.processedOutputColor, rg::TextureAccess::UnorderedAccess),
        .depth = graph.importTexture(targets.depth, rg::TextureAccess::UnorderedAccess),
        .motionVectors = graph.importTexture(targets.screenMotionVectors, rg::TextureAccess::UnorderedAccess),
        .throughput = graph.importTexture(targets.throughput, rg::TextureAccess::UnorderedAccess),
        .specularHitT = graph.importTexture(targets.specularHitT, rg::TextureAccess::UnorderedAccess),
        .scratchFloat1 = graph.importTexture(targets.scratchFloat1, rg::TextureAccess::UnorderedAccess),
        .stableRadiance = graph.importTexture(targets.stableRadiance, rg::TextureAccess::UnorderedAccess),
        .stablePlanesHeader = graph.importTexture(targets.stablePlanesHeader, rg::TextureAccess::UnorderedAccess),
        .stablePlanesBuffer = graph.importBuffer(targets.stablePlanesBuffer, rg::BufferAccess::UnorderedAccess),
        .denoiserViewspaceZ = graph.importTexture(targets.denoiserViewspaceZ, rg::TextureAccess::UnorderedAccess),
        .denoiserMotionVectors = graph.importTexture(targets.denoiserMotionVectors, rg::TextureAccess::UnorderedAccess),
        .denoiserNormalRoughness = graph.importTexture(targets.denoiserNormalRoughness, rg::TextureAccess::UnorderedAccess),
        .denoiserDiffRadianceHitDist = graph.importTexture(targets.denoiserDiffRadianceHitDist, rg::TextureAccess::UnorderedAccess),
        .denoiserSpecRadianceHitDist = graph.importTexture(targets.denoiserSpecRadianceHitDist, rg::TextureAccess::UnorderedAccess),
        .denoiserDisocclusionThresholdMix = graph.importTexture(
            targets.denoiserDisocclusionThresholdMix,
            rg::TextureAccess::UnorderedAccess),
        .denoiserAvgLayerRadianceHalfRes = graph.importTexture(
            targets.denoiserAvgLayerRadianceHalfRes,
            rg::TextureAccess::UnorderedAccess),
        .baseColor = graph.importTexture(targets.baseColor, rg::TextureAccess::UnorderedAccess),
        .specNormal = graph.importTexture(targets.specNormal, rg::TextureAccess::UnorderedAccess),
        .roughnessMetal = graph.importTexture(targets.roughnessMetal, rg::TextureAccess::UnorderedAccess),
        .materialInfo = graph.importTexture(targets.materialInfo, rg::TextureAccess::UnorderedAccess),
        .surfaceDataBuffer = graph.importBuffer(targets.surfaceDataBuffer, rg::BufferAccess::UnorderedAccess),
        .secondarySurfacePositionNormal = graph.importTexture(
            targets.secondarySurfacePositionNormal,
            rg::TextureAccess::UnorderedAccess),
        .secondarySurfaceRadiance = graph.importTexture(
            targets.secondarySurfaceRadiance,
            rg::TextureAccess::UnorderedAccess),
    };
}

void extractPathTraceGraphOutputs(rg::GraphBuilder& graph, const PathTraceGraphTargets& handles)
{
    graph.extractTexture(handles.outputColor, rg::TextureAccess::UnorderedAccess);
    graph.extractTexture(handles.processedOutputColor, rg::TextureAccess::UnorderedAccess);
    graph.extractTexture(handles.depth, rg::TextureAccess::UnorderedAccess);
    graph.extractTexture(handles.motionVectors, rg::TextureAccess::UnorderedAccess);
}

void declarePathTraceOutputWrites(rg::PassBuilder& setup, const PathTraceGraphTargets& handles)
{
    // Path tracer and RTXDI bindings use UAV slots (RWTexture/RWBuffer), not SRV.
    setup.write(handles.outputColor, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.processedOutputColor, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.depth, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.motionVectors, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.throughput, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.specularHitT, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.stableRadiance, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.stablePlanesHeader, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.stablePlanesBuffer, rg::BufferAccess::UnorderedAccess);
    setup.write(handles.denoiserViewspaceZ, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.denoiserMotionVectors, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.denoiserNormalRoughness, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.denoiserDiffRadianceHitDist, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.denoiserSpecRadianceHitDist, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.denoiserDisocclusionThresholdMix, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.denoiserAvgLayerRadianceHalfRes, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.baseColor, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.specNormal, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.roughnessMetal, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.materialInfo, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.surfaceDataBuffer, rg::BufferAccess::UnorderedAccess);
}

void declarePathTraceLightingEndAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles)
{
    // LightSamplingCache uses t_depthBuffer / t_motionVectors SRV slots.
    setup.read(handles.depth, rg::TextureAccess::ShaderResource);
    setup.read(handles.motionVectors, rg::TextureAccess::ShaderResource);
}

void declarePathTracePrePassAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles)
{
    declarePathTraceOutputWrites(setup, handles);
}

void declareVBufferExportAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles)
{
    declarePathTraceOutputWrites(setup, handles);
}

void declareMainPathTraceAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles)
{
    declarePathTraceOutputWrites(setup, handles);
    setup.write(handles.secondarySurfacePositionNormal, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.secondarySurfaceRadiance, rg::TextureAccess::UnorderedAccess);
}

void declareDenoiserPrepareAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles)
{
    setup.write(handles.depth, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.specularHitT, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.scratchFloat1, rg::TextureAccess::UnorderedAccess);
    setup.write(handles.denoiserAvgLayerRadianceHalfRes, rg::TextureAccess::UnorderedAccess);
}

void declareStablePlanesDebugVizAccess(rg::PassBuilder& setup, const PathTraceGraphTargets& handles)
{
    declarePathTraceOutputWrites(setup, handles);
}

bool needsPathTraceLightingEndPass(const PathTracerSettings& settings)
{
    return settings.NEEType == 2;
}

const char* pathTraceLightingEndExecuteAfterPass(const PathTracerSettings& settings)
{
    return settings.RealtimeMode ? "VBufferExport" : kLightingReadyPass;
}

const char* pathTraceMainExecuteAfterPass(const PathTracerSettings& settings)
{
    if (needsGaussianSplatAccelBuild(settings))
        return "GaussianSplatsAccelBuild";

    if (needsPathTraceLightingEndPass(settings))
        return "PathTraceLightingEnd";

    return settings.RealtimeMode ? "VBufferExport" : kLightingReadyPass;
}

void validateReferencePathTraceGraph(const rg::GraphBuilder& graph, const PathTracerSettings& settings)
{
    assert(graph.isCompiled());
    assert(!settings.RealtimeMode);

    assert(!graph.isPassRegistered("PathTracePrePass"));
    assert(!graph.isPassRegistered("VBufferExport"));

    if (!settings.actualUseRTXDIPasses())
    {
        assert(!graph.isPassRegistered(kRtxdiFillConstantsPass));
        assert(!graph.isPassRegistered(kRtxdiDIPass));
    }

    assert(graph.isPassActive(kLightingReadyPass));
    assert(graph.isPassActive("MainPathTrace"));

    if (needsPathTraceLightingEndPass(settings))
        assert(graph.isPassActive("PathTraceLightingEnd"));
    else
        assert(!graph.isPassRegistered("PathTraceLightingEnd"));
}

} // namespace caustica::render

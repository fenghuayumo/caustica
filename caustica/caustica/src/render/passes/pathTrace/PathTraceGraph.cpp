#include <render/passes/pathTrace/PathTraceGraph.h>

#include <render/core/RenderTargets.h>
#include <render/FramePassRegistry.h>
#include <render/passes/rtxdi/RtxdiPass.h>
#include <render/worldRenderer/PathTracingFrameContext.h>
#include <render/worldRenderer/WorldRenderer.h>

#include <cassert>

namespace caustica::rg
{

namespace
{
    void declarePathTraceWrites(PassBuilder& setup, const PathTraceGraphParams& params)
    {
        RenderTargets* targets = params.renderTargets;
        assert(targets);

        const TextureHandle outputColor = params.graph.importTexture(
            targets->outputColor, TextureAccess::UnorderedAccess);
        const TextureHandle processedOutputColor = params.graph.importTexture(
            targets->processedOutputColor, TextureAccess::UnorderedAccess);
        const TextureHandle depth = params.graph.importTexture(
            targets->depth, TextureAccess::UnorderedAccess);
        const TextureHandle motionVectors = params.graph.importTexture(
            targets->screenMotionVectors, TextureAccess::UnorderedAccess);
        const TextureHandle throughput = params.graph.importTexture(
            targets->throughput, TextureAccess::UnorderedAccess);
        const TextureHandle specularHitT = params.graph.importTexture(
            targets->specularHitT, TextureAccess::UnorderedAccess);
        const TextureHandle stableRadiance = params.graph.importTexture(
            targets->stableRadiance, TextureAccess::UnorderedAccess);
        const TextureHandle stablePlanesHeader = params.graph.importTexture(
            targets->stablePlanesHeader, TextureAccess::UnorderedAccess);
        const BufferHandle stablePlanesBuffer = params.graph.importBuffer(
            targets->stablePlanesBuffer, BufferAccess::UnorderedAccess);
        const TextureHandle denoiserViewspaceZ = params.graph.importTexture(
            targets->denoiserViewspaceZ, TextureAccess::UnorderedAccess);
        const TextureHandle denoiserMotionVectors = params.graph.importTexture(
            targets->denoiserMotionVectors, TextureAccess::UnorderedAccess);
        const TextureHandle denoiserNormalRoughness = params.graph.importTexture(
            targets->denoiserNormalRoughness, TextureAccess::UnorderedAccess);
        const TextureHandle denoiserDiffRadianceHitDist = params.graph.importTexture(
            targets->denoiserDiffRadianceHitDist, TextureAccess::UnorderedAccess);
        const TextureHandle denoiserSpecRadianceHitDist = params.graph.importTexture(
            targets->denoiserSpecRadianceHitDist, TextureAccess::UnorderedAccess);
        const TextureHandle denoiserDisocclusionThresholdMix = params.graph.importTexture(
            targets->denoiserDisocclusionThresholdMix, TextureAccess::UnorderedAccess);
        const TextureHandle denoiserAvgLayerRadianceHalfRes = params.graph.importTexture(
            targets->denoiserAvgLayerRadianceHalfRes, TextureAccess::UnorderedAccess);
        const TextureHandle baseColor = params.graph.importTexture(
            targets->baseColor, TextureAccess::UnorderedAccess);
        const TextureHandle specNormal = params.graph.importTexture(
            targets->specNormal, TextureAccess::UnorderedAccess);
        const TextureHandle roughnessMetal = params.graph.importTexture(
            targets->roughnessMetal, TextureAccess::UnorderedAccess);
        const TextureHandle materialInfo = params.graph.importTexture(
            targets->materialInfo, TextureAccess::UnorderedAccess);
        const BufferHandle surfaceDataBuffer = params.graph.importBuffer(
            targets->surfaceDataBuffer, BufferAccess::UnorderedAccess);

        params.graph.extractTexture(outputColor, TextureAccess::UnorderedAccess);
        params.graph.extractTexture(processedOutputColor, TextureAccess::UnorderedAccess);
        params.graph.extractTexture(depth, TextureAccess::UnorderedAccess);
        params.graph.extractTexture(motionVectors, TextureAccess::UnorderedAccess);

        setup.write(outputColor, TextureAccess::UnorderedAccess);
        setup.write(processedOutputColor, TextureAccess::UnorderedAccess);
        setup.write(depth, TextureAccess::UnorderedAccess);
        setup.write(motionVectors, TextureAccess::UnorderedAccess);
        setup.write(throughput, TextureAccess::UnorderedAccess);
        setup.write(specularHitT, TextureAccess::UnorderedAccess);
        setup.write(stableRadiance, TextureAccess::UnorderedAccess);
        setup.write(stablePlanesHeader, TextureAccess::UnorderedAccess);
        setup.write(stablePlanesBuffer, BufferAccess::UnorderedAccess);
        setup.write(denoiserViewspaceZ, TextureAccess::UnorderedAccess);
        setup.write(denoiserMotionVectors, TextureAccess::UnorderedAccess);
        setup.write(denoiserNormalRoughness, TextureAccess::UnorderedAccess);
        setup.write(denoiserDiffRadianceHitDist, TextureAccess::UnorderedAccess);
        setup.write(denoiserSpecRadianceHitDist, TextureAccess::UnorderedAccess);
        setup.write(denoiserDisocclusionThresholdMix, TextureAccess::UnorderedAccess);
        setup.write(denoiserAvgLayerRadianceHalfRes, TextureAccess::UnorderedAccess);
        setup.write(baseColor, TextureAccess::UnorderedAccess);
        setup.write(specNormal, TextureAccess::UnorderedAccess);
        setup.write(roughnessMetal, TextureAccess::UnorderedAccess);
        setup.write(materialInfo, TextureAccess::UnorderedAccess);
        setup.write(surfaceDataBuffer, BufferAccess::UnorderedAccess);
    }
}

void buildPathTraceGraph(const PathTraceGraphParams& params)
{
    assert(params.worldRenderer);
    assert(params.renderTargets);
    assert(params.settings);
    assert(params.sampleConstants);

    if (!params.hasScene)
        return;

    GraphBuilder& graph = params.graph;

    graph.addPass(
        "PathTrace",
        [&params](PassBuilder& setup) {
            declarePathTraceWrites(setup, params);
        },
        [params](RenderPassContext& ctx) {
            if (params.settings->ActualUseRTXDIPasses())
            {
                RtxdiPass* rtxdiPass = params.worldRenderer->getRtxdiPass();
                if (rtxdiPass != nullptr)
                {
                    rtxdiPass->BeginFrame(
                        ctx.commandList(),
                        *params.renderTargets,
                        params.worldRenderer->getBindingLayout(),
                        params.worldRenderer->getBindingSet());
                }
            }

            params.worldRenderer->pathTrace(
                ctx.commandList(),
                params.framebuffer,
                *params.sampleConstants);
        },
        PassOptions{ .sideEffect = true });

    if (params.framePassRegistry && params.frameContext)
    {
        params.framePassRegistry->applyGraphPasses(
            render::FramePassInsertPoint::AfterPathTrace,
            graph,
            *params.frameContext);
    }
}

} // namespace caustica::rg

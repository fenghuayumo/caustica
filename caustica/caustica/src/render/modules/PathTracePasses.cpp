#include <render/modules/PathTracePasses.h>

#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/modules/RenderModuleContext.h>
#include <render/passes/rtxdi/RtxdiPass.h>
#include <render/worldRenderer/WorldRenderer.h>

#include <cassert>

namespace caustica::render
{

namespace
{
    void declarePathTraceWrites(rg::PassBuilder& setup, const RenderModuleContext& ctx)
    {
        assert(ctx.renderTargets);
        assert(ctx.graph);

        RenderTargets& targets = *ctx.renderTargets;
        rg::GraphBuilder& graph = *ctx.graph;

        const rg::TextureHandle outputColor = graph.importTexture(
            targets.outputColor, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle processedOutputColor = graph.importTexture(
            targets.processedOutputColor, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle depth = graph.importTexture(
            targets.depth, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle motionVectors = graph.importTexture(
            targets.screenMotionVectors, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle throughput = graph.importTexture(
            targets.throughput, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle specularHitT = graph.importTexture(
            targets.specularHitT, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle stableRadiance = graph.importTexture(
            targets.stableRadiance, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle stablePlanesHeader = graph.importTexture(
            targets.stablePlanesHeader, rg::TextureAccess::UnorderedAccess);
        const rg::BufferHandle stablePlanesBuffer = graph.importBuffer(
            targets.stablePlanesBuffer, rg::BufferAccess::UnorderedAccess);
        const rg::TextureHandle denoiserViewspaceZ = graph.importTexture(
            targets.denoiserViewspaceZ, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle denoiserMotionVectors = graph.importTexture(
            targets.denoiserMotionVectors, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle denoiserNormalRoughness = graph.importTexture(
            targets.denoiserNormalRoughness, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle denoiserDiffRadianceHitDist = graph.importTexture(
            targets.denoiserDiffRadianceHitDist, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle denoiserSpecRadianceHitDist = graph.importTexture(
            targets.denoiserSpecRadianceHitDist, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle denoiserDisocclusionThresholdMix = graph.importTexture(
            targets.denoiserDisocclusionThresholdMix, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle denoiserAvgLayerRadianceHalfRes = graph.importTexture(
            targets.denoiserAvgLayerRadianceHalfRes, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle baseColor = graph.importTexture(
            targets.baseColor, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle specNormal = graph.importTexture(
            targets.specNormal, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle roughnessMetal = graph.importTexture(
            targets.roughnessMetal, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle materialInfo = graph.importTexture(
            targets.materialInfo, rg::TextureAccess::UnorderedAccess);
        const rg::BufferHandle surfaceDataBuffer = graph.importBuffer(
            targets.surfaceDataBuffer, rg::BufferAccess::UnorderedAccess);

        graph.extractTexture(outputColor, rg::TextureAccess::UnorderedAccess);
        graph.extractTexture(processedOutputColor, rg::TextureAccess::UnorderedAccess);
        graph.extractTexture(depth, rg::TextureAccess::UnorderedAccess);
        graph.extractTexture(motionVectors, rg::TextureAccess::UnorderedAccess);

        setup.write(outputColor, rg::TextureAccess::UnorderedAccess);
        setup.write(processedOutputColor, rg::TextureAccess::UnorderedAccess);
        setup.write(depth, rg::TextureAccess::UnorderedAccess);
        setup.write(motionVectors, rg::TextureAccess::UnorderedAccess);
        setup.write(throughput, rg::TextureAccess::UnorderedAccess);
        setup.write(specularHitT, rg::TextureAccess::UnorderedAccess);
        setup.write(stableRadiance, rg::TextureAccess::UnorderedAccess);
        setup.write(stablePlanesHeader, rg::TextureAccess::UnorderedAccess);
        setup.write(stablePlanesBuffer, rg::BufferAccess::UnorderedAccess);
        setup.write(denoiserViewspaceZ, rg::TextureAccess::UnorderedAccess);
        setup.write(denoiserMotionVectors, rg::TextureAccess::UnorderedAccess);
        setup.write(denoiserNormalRoughness, rg::TextureAccess::UnorderedAccess);
        setup.write(denoiserDiffRadianceHitDist, rg::TextureAccess::UnorderedAccess);
        setup.write(denoiserSpecRadianceHitDist, rg::TextureAccess::UnorderedAccess);
        setup.write(denoiserDisocclusionThresholdMix, rg::TextureAccess::UnorderedAccess);
        setup.write(denoiserAvgLayerRadianceHalfRes, rg::TextureAccess::UnorderedAccess);
        setup.write(baseColor, rg::TextureAccess::UnorderedAccess);
        setup.write(specNormal, rg::TextureAccess::UnorderedAccess);
        setup.write(roughnessMetal, rg::TextureAccess::UnorderedAccess);
        setup.write(materialInfo, rg::TextureAccess::UnorderedAccess);
        setup.write(surfaceDataBuffer, rg::BufferAccess::UnorderedAccess);
    }
}

void registerPathTracePasses(RenderModuleContext ctx)
{
    assert(ctx.targetFramebuffer);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);
    assert(ctx.sampleConstants);

    if (!ctx.hasScene)
        return;

    ctx.graph->addPass(
        "PathTrace",
        [ctx](rg::PassBuilder& setup) {
            declarePathTraceWrites(setup, ctx);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            WorldRenderer& renderer = *ctx.renderer;
            if (ctx.settings->ActualUseRTXDIPasses())
            {
                RtxdiPass* rtxdiPass = renderer.getRtxdiPass();
                if (rtxdiPass != nullptr)
                {
                    rtxdiPass->BeginFrame(
                        passCtx.commandList(),
                        *ctx.renderTargets,
                        renderer.getBindingLayout(),
                        renderer.getBindingSet());
                }
            }

            renderer.pathTrace(
                passCtx.commandList(),
                ctx.targetFramebuffer,
                *ctx.sampleConstants);
        },
        rg::PassOptions{ .sideEffect = true });
}

} // namespace caustica::render

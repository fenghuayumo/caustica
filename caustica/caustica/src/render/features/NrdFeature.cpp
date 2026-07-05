#include <render/features/NrdFeature.h>

#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/features/RenderFeatureContext.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <shaders/PathTracer/Config.h>

#include <cassert>
#include <format>

namespace caustica::render
{

namespace
{
    void declareNrdPlaneAccess(
        rg::PassBuilder& setup,
        rg::GraphBuilder& graph,
        RenderTargets& targets,
        int planeIndex,
        bool readsOutputColor)
    {
        const rg::TextureHandle denoiserViewspaceZ = graph.importTexture(
            targets.denoiserViewspaceZ, rg::TextureAccess::ShaderResource);
        const rg::TextureHandle denoiserMotionVectors = graph.importTexture(
            targets.denoiserMotionVectors, rg::TextureAccess::ShaderResource);
        const rg::TextureHandle denoiserNormalRoughness = graph.importTexture(
            targets.denoiserNormalRoughness, rg::TextureAccess::ShaderResource);
        const rg::TextureHandle denoiserDiffRadianceHitDist = graph.importTexture(
            targets.denoiserDiffRadianceHitDist, rg::TextureAccess::ShaderResource);
        const rg::TextureHandle denoiserSpecRadianceHitDist = graph.importTexture(
            targets.denoiserSpecRadianceHitDist, rg::TextureAccess::ShaderResource);
        const rg::TextureHandle denoiserDisocclusionThresholdMix = graph.importTexture(
            targets.denoiserDisocclusionThresholdMix, rg::TextureAccess::ShaderResource);
        const rg::TextureHandle outputColor = graph.importTexture(
            targets.outputColor, rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle outDiff = graph.importTexture(
            targets.denoiserOutDiffRadianceHitDist[planeIndex], rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle outSpec = graph.importTexture(
            targets.denoiserOutSpecRadianceHitDist[planeIndex], rg::TextureAccess::UnorderedAccess);

        graph.extractTexture(outputColor, rg::TextureAccess::UnorderedAccess);
        graph.extractTexture(outDiff, rg::TextureAccess::UnorderedAccess);
        graph.extractTexture(outSpec, rg::TextureAccess::UnorderedAccess);

        setup.read(denoiserViewspaceZ, rg::TextureAccess::ShaderResource);
        setup.read(denoiserMotionVectors, rg::TextureAccess::ShaderResource);
        setup.read(denoiserNormalRoughness, rg::TextureAccess::ShaderResource);
        setup.read(denoiserDiffRadianceHitDist, rg::TextureAccess::ShaderResource);
        setup.read(denoiserSpecRadianceHitDist, rg::TextureAccess::ShaderResource);
        setup.read(denoiserDisocclusionThresholdMix, rg::TextureAccess::ShaderResource);
        if (readsOutputColor)
            setup.read(outputColor, rg::TextureAccess::ShaderResource);
        setup.write(outDiff, rg::TextureAccess::UnorderedAccess);
        setup.write(outSpec, rg::TextureAccess::UnorderedAccess);
        setup.write(outputColor, rg::TextureAccess::UnorderedAccess);

        if (targets.denoiserOutValidation != nullptr)
        {
            const rg::TextureHandle validation = graph.importTexture(
                targets.denoiserOutValidation, rg::TextureAccess::UnorderedAccess);
            setup.write(validation, rg::TextureAccess::UnorderedAccess);
        }
    }
}

void registerNrdFeature(RenderFeatureContext ctx)
{
    assert(ctx.targetFramebuffer);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);
    assert(ctx.graph);

    if (!ctx.hasScene || !ctx.settings->ActualUseStandaloneDenoiser())
        return;

    ctx.renderer->ensureNrdIntegrations();

    const int maxPassCount = std::min(
        ctx.settings->StablePlanesActiveCount,
        static_cast<int>(cStablePlaneCount));

    for (int pass = maxPassCount - 1; pass >= 0; --pass)
    {
        const int planeIndex = pass;
        const bool readsOutputColor = planeIndex < (maxPassCount - 1);
        ctx.graph->addPass(
            std::format("NRD Plane {}", planeIndex),
            [ctx, planeIndex, readsOutputColor](rg::PassBuilder& setup) {
                declareNrdPlaneAccess(setup, *ctx.graph, *ctx.renderTargets, planeIndex, readsOutputColor);
            },
            [ctx, planeIndex](rg::RenderPassContext& passCtx) {
                ctx.renderer->denoiseStablePlane(
                    passCtx.commandList(),
                    ctx.targetFramebuffer,
                    planeIndex);
            });
    }
}

} // namespace caustica::render

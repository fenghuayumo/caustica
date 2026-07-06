#include <render/features/RenderFeature.h>

#include <render/features/RenderFeatureContext.h>
#include <render/features/PathTraceGraphResources.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/gaussian/GaussianSplatGraph.h>
#include <render/worldRenderer/WorldRenderer.h>

#include <cassert>

namespace caustica::render
{

namespace
{
    bool gaussianSplatsEnabled(const RenderFeatureContext& ctx)
    {
        return ctx.settings != nullptr
            && ctx.renderer != nullptr
            && ctx.settings->EnableGaussianSplats
            && ctx.renderer->hasActiveGaussianSplats();
    }
}

void registerGaussianSplatPreAAFeature(RenderFeatureContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!needsStochasticGaussianSplatsBeforeAA(*ctx.settings))
        return;

    RenderTargets& targets = *ctx.renderTargets;

    const rg::TextureHandle outputColor = ctx.graph->importTexture(
        targets.outputColor,
        rg::TextureAccess::UnorderedAccess);
    const rg::TextureHandle depth = ctx.graph->importTexture(
        targets.depth,
        rg::TextureAccess::ShaderResource);

    ctx.graph->addPass(
        "GaussianSplatsStochastic",
        [&](rg::PassBuilder& setup) {
            setup.read(outputColor, rg::TextureAccess::UnorderedAccess);
            setup.read(depth, rg::TextureAccess::ShaderResource);
            setup.write(outputColor, rg::TextureAccess::UnorderedAccess);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            if (!gaussianSplatsEnabled(ctx))
                return;
            ctx.renderer->executeGaussianSplatRender(passCtx.commandList(), true);
        },
        rg::PassOptions{ .sideEffect = true });
}

void registerGaussianSplatAccelBuildFeature(RenderFeatureContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.settings);

    if (!needsGaussianSplatAccelBuild(*ctx.settings))
        return;

    rg::PassOptions passOptions{ .sideEffect = true };
    if (needsPathTraceLightingEndPass(*ctx.settings))
        passOptions.executeAfter = "PathTraceLightingEnd";
    else
        passOptions.executeAfter = ctx.settings->RealtimeMode ? "VBufferExport" : "FrameClear";

    ctx.graph->addPass(
        "GaussianSplatsAccelBuild",
        [](rg::PassBuilder& setup) {
            (void)setup;
        },
        [ctx](rg::RenderPassContext& passCtx) {
            if (!gaussianSplatsEnabled(ctx))
                return;
            ctx.renderer->executeGaussianSplatAccelBuild(passCtx.commandList());
        },
        passOptions);
}

void registerGaussianSplatCompositeFeature(RenderFeatureContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!needsGaussianSplatsCompositePass(*ctx.settings))
        return;

    RenderTargets& targets = *ctx.renderTargets;

    const rg::TextureHandle processedOutputColor = ctx.graph->importTexture(
        targets.processedOutputColor,
        rg::TextureAccess::UnorderedAccess);
    const rg::TextureHandle depth = ctx.graph->importTexture(
        targets.depth,
        rg::TextureAccess::ShaderResource);

    ctx.graph->addPass(
        "GaussianSplatsComposite",
        [&](rg::PassBuilder& setup) {
            setup.read(processedOutputColor, rg::TextureAccess::UnorderedAccess);
            setup.read(depth, rg::TextureAccess::ShaderResource);
            setup.write(processedOutputColor, rg::TextureAccess::UnorderedAccess);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            if (!gaussianSplatsEnabled(ctx))
                return;
            ctx.renderer->executeGaussianSplatRender(passCtx.commandList(), false);
        },
        rg::PassOptions{ .sideEffect = true });

    if (!needsGaussianSplatStochasticAccumulate(*ctx.settings))
        return;

    ctx.graph->addPass(
        "GaussianSplatsAccumulate",
        [&](rg::PassBuilder& setup) {
            setup.read(processedOutputColor, rg::TextureAccess::UnorderedAccess);
            setup.write(processedOutputColor, rg::TextureAccess::UnorderedAccess);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            if (!gaussianSplatsEnabled(ctx))
                return;
            ctx.renderer->executeGaussianSplatAccumulate(passCtx.commandList());
        },
        rg::PassOptions{
            .sideEffect = true,
            .executeAfter = "GaussianSplatsComposite",
        });
}

} // namespace caustica::render

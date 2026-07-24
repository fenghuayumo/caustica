#include <render/FrameGraphPasses.h>

#include <render/FrameGraphContext.h>
#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/gaussian/GaussianSplatFramePass.h>
#include <render/passes/gaussian/GaussianSplatGraph.h>
#include <render/passes/pathTrace/PathTraceGraphResources.h>

#include <cassert>
#include <vector>

namespace caustica::render
{

namespace
{
    struct GaussianSplatGraphHandles
    {
        rg::BufferHandle constantBuffer;
        rg::BufferHandle splatBuffer;
        rg::BufferHandle colorBuffer;
        rg::BufferHandle shBuffer;
        rg::BufferHandle indexBuffer;
        rg::BufferHandle sortKeyBuffer;
        rg::BufferHandle sortControlBuffer;
        rg::BufferHandle drawIndirectBuffer;
        rg::TextureHandle stochasticDepth;
        bool hasStochasticDepth = false;
        GaussianSplatSortMode sortMode = GaussianSplatSortMode::GpuSort;
        bool distanceStageCulling = false;
    };

    bool gaussianSplatsEnabled(const FrameGraphContext& ctx)
    {
        return ctx.settings != nullptr
            && ctx.gaussian != nullptr
            && ctx.settings->EnableGaussianSplats
            && ctx.gaussian->hasActiveSplats();
    }

    std::vector<GaussianSplatGraphHandles> importGaussianSplatGraphResources(
        FrameGraphContext ctx,
        bool renderToOutputColor)
    {
        std::vector<GaussianSplatGraphHandles> handles;
        if (!ctx.gaussian)
            return handles;

        for (const GaussianSplatGraphResources& resources :
            ctx.gaussian->prepareGraphResources(renderToOutputColor))
        {
            if (!resources.constantBuffer || !resources.splatBuffer
                || !resources.colorBuffer || !resources.shBuffer
                || !resources.indexBuffer || !resources.sortKeyBuffer
                || !resources.sortControlBuffer || !resources.drawIndirectBuffer)
            {
                continue;
            }

            GaussianSplatGraphHandles imported{
                .constantBuffer = ctx.graph->importBuffer(resources.constantBuffer, rg::BufferAccess::ConstantBuffer),
                .splatBuffer = ctx.graph->importBuffer(resources.splatBuffer, rg::BufferAccess::ShaderResource),
                .colorBuffer = ctx.graph->importBuffer(resources.colorBuffer, rg::BufferAccess::ShaderResource),
                .shBuffer = ctx.graph->importBuffer(resources.shBuffer, rg::BufferAccess::ShaderResource),
                .indexBuffer = ctx.graph->importBuffer(resources.indexBuffer, rg::BufferAccess::UnorderedAccess),
                .sortKeyBuffer = ctx.graph->importBuffer(resources.sortKeyBuffer, rg::BufferAccess::UnorderedAccess),
                .sortControlBuffer = ctx.graph->importBuffer(resources.sortControlBuffer, rg::BufferAccess::UnorderedAccess),
                .drawIndirectBuffer = ctx.graph->importBuffer(resources.drawIndirectBuffer, rg::BufferAccess::UnorderedAccess),
                .sortMode = resources.sortMode,
                .distanceStageCulling = resources.distanceStageCulling,
            };
            if (resources.stochasticDepth)
            {
                imported.stochasticDepth = ctx.graph->importTexture(
                    resources.stochasticDepth,
                    rg::TextureAccess::DepthWrite);
                imported.hasStochasticDepth = true;
            }
            handles.push_back(imported);
        }
        return handles;
    }

    bool registerGaussianSplatRenderStages(
        FrameGraphContext ctx,
        bool renderToOutputColor,
        rg::TextureHandle colorTarget,
        rg::TextureHandle sceneDepth,
        const char* uploadPassName,
        const char* sortPassName,
        const char* rasterPassName)
    {
        const std::vector<GaussianSplatGraphHandles> resources =
            importGaussianSplatGraphResources(ctx, renderToOutputColor);
        if (resources.empty())
            return false;

        ctx.graph->addPass(
            uploadPassName,
            [resources](rg::PassBuilder& setup) {
                for (const GaussianSplatGraphHandles& item : resources)
                {
                    setup.write(item.constantBuffer, rg::BufferAccess::CopyDest);
                    setup.write(item.splatBuffer, rg::BufferAccess::CopyDest);
                    setup.write(item.colorBuffer, rg::BufferAccess::CopyDest);
                    setup.write(item.shBuffer, rg::BufferAccess::CopyDest);
                }
            },
            [ctx, renderToOutputColor](rg::RenderPassContext& passCtx) {
                if (gaussianSplatsEnabled(ctx))
                    ctx.gaussian->executeUpload(passCtx.commandList(), renderToOutputColor);
            },
            rg::PassOptions{ .sideEffect = true });

        ctx.graph->addPass(
            sortPassName,
            [resources](rg::PassBuilder& setup) {
                for (const GaussianSplatGraphHandles& item : resources)
                {
                    if (item.sortMode == GaussianSplatSortMode::StochasticSplats
                        && !item.distanceStageCulling)
                    {
                        setup.write(item.indexBuffer, rg::BufferAccess::CopyDest);
                    }
                    else
                    {
                        setup.read(item.constantBuffer, rg::BufferAccess::ConstantBuffer);
                        setup.read(item.splatBuffer, rg::BufferAccess::ShaderResource);
                        setup.write(item.indexBuffer, rg::BufferAccess::UnorderedAccess);
                        setup.write(item.sortKeyBuffer, rg::BufferAccess::UnorderedAccess);
                        setup.write(item.sortControlBuffer, rg::BufferAccess::UnorderedAccess);
                        setup.write(item.drawIndirectBuffer, rg::BufferAccess::UnorderedAccess);
                    }
                }
            },
            [ctx](rg::RenderPassContext& passCtx) {
                if (gaussianSplatsEnabled(ctx))
                    ctx.gaussian->executeSort(passCtx.commandList());
            },
            rg::PassOptions{ .sideEffect = true, .executeAfter = uploadPassName });

        ctx.graph->addPass(
            rasterPassName,
            [resources, colorTarget, sceneDepth](rg::PassBuilder& setup) {
                setup.read(sceneDepth, rg::TextureAccess::ShaderResource);
                setup.write(colorTarget, rg::TextureAccess::RenderTarget);
                for (const GaussianSplatGraphHandles& item : resources)
                {
                    setup.read(item.constantBuffer, rg::BufferAccess::ConstantBuffer);
                    setup.read(item.splatBuffer, rg::BufferAccess::ShaderResource);
                    setup.read(item.colorBuffer, rg::BufferAccess::ShaderResource);
                    setup.read(item.shBuffer, rg::BufferAccess::ShaderResource);
                    setup.read(item.indexBuffer, rg::BufferAccess::ShaderResource);
                    if (item.distanceStageCulling)
                        setup.read(item.drawIndirectBuffer, rg::BufferAccess::IndirectArgument);
                    if (item.hasStochasticDepth)
                        setup.write(item.stochasticDepth, rg::TextureAccess::DepthWrite);
                }
            },
            [ctx, renderToOutputColor](rg::RenderPassContext& passCtx) {
                if (gaussianSplatsEnabled(ctx))
                    ctx.gaussian->executeRaster(passCtx.commandList(), renderToOutputColor);
            },
            rg::PassOptions{ .sideEffect = true, .executeAfter = sortPassName });
        return true;
    }
}

void registerGaussianSplatPreAAPass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.gaussian);
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

    (void)registerGaussianSplatRenderStages(
        ctx,
        true,
        outputColor,
        depth,
        "GaussianSplatsStochasticUpload",
        "GaussianSplatsStochasticSort",
        "GaussianSplatsStochastic");
}

void registerGaussianSplatAccelBuildPass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.gaussian);
    assert(ctx.settings);

    if (!needsGaussianSplatAccelBuild(*ctx.settings))
        return;

    rg::PassOptions passOptions{ .sideEffect = true };
    if (needsPathTraceLightingEndPass(*ctx.settings))
        passOptions.executeAfter = "PathTraceLightingEnd";
    else
        passOptions.executeAfter = ctx.settings->RealtimeMode ? "VBufferExport" : "LightingUpdateBegin";

    ctx.graph->addPass(
        "GaussianSplatsAccelBuild",
        [](rg::PassBuilder& setup) {
            (void)setup;
        },
        [ctx](rg::RenderPassContext& passCtx) {
            if (!gaussianSplatsEnabled(ctx))
                return;
            ctx.gaussian->executeAccelBuild(passCtx.commandList());
        },
        passOptions);
}

void registerGaussianSplatCompositePass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.gaussian);
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

    const bool registeredRenderStages = registerGaussianSplatRenderStages(
        ctx,
        false,
        processedOutputColor,
        depth,
        "GaussianSplatsCompositeUpload",
        "GaussianSplatsCompositeSort",
        "GaussianSplatsComposite");
    if (!registeredRenderStages)
        return;

    if (!needsGaussianSplatStochasticAccumulate(*ctx.settings))
        return;

    caustica::rhi::ITexture* currentColorTexture = ctx.gaussian->currentColor();
    caustica::rhi::ITexture* accumulatedColorTexture = ctx.gaussian->accumulatedColor();
    if (currentColorTexture == nullptr || accumulatedColorTexture == nullptr)
        return;

    const rg::TextureHandle currentColor = ctx.graph->importTexture(
        currentColorTexture,
        rg::TextureAccess::ShaderResource);
    const rg::TextureHandle accumulatedColor = ctx.graph->importTexture(
        accumulatedColorTexture,
        rg::TextureAccess::UnorderedAccess);

    ctx.graph->addPass(
        "GaussianSplatsCopyCurrent",
        [processedOutputColor, currentColor](rg::PassBuilder& setup) {
            setup.read(processedOutputColor, rg::TextureAccess::CopySource);
            setup.write(currentColor, rg::TextureAccess::CopyDest);
        },
        [ctx, processedOutputColor, currentColor](rg::RenderPassContext& passCtx) {
            if (!gaussianSplatsEnabled(ctx))
                return;
            passCtx.commandList()->copyTexture(
                passCtx.texture(currentColor), caustica::rhi::TextureSlice(),
                passCtx.texture(processedOutputColor), caustica::rhi::TextureSlice());
        },
        rg::PassOptions{
            .sideEffect = true,
            .executeAfter = "GaussianSplatsComposite",
        });

    ctx.graph->addPass(
        "GaussianSplatsAccumulate",
        [processedOutputColor, currentColor, accumulatedColor](rg::PassBuilder& setup) {
            setup.read(currentColor, rg::TextureAccess::ShaderResource);
            setup.read(accumulatedColor, rg::TextureAccess::UnorderedAccess);
            setup.write(accumulatedColor, rg::TextureAccess::UnorderedAccess);
            setup.write(processedOutputColor, rg::TextureAccess::UnorderedAccess);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            if (!gaussianSplatsEnabled(ctx))
                return;
            ctx.gaussian->executeAccumulate(passCtx.commandList());
        },
        rg::PassOptions{
            .sideEffect = true,
            .executeAfter = "GaussianSplatsCopyCurrent",
        });
}

void registerGaussianSplatGraphPasses(FrameGraphContext ctx)
{
    registerGaussianSplatAccelBuildPass(ctx);
    registerGaussianSplatPreAAPass(ctx);
    registerGaussianSplatCompositePass(ctx);
}

} // namespace caustica::render

#include <render/FrameGraphPasses.h>

#include <render/FrameGraphContext.h>
#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/gaussian/GaussianSplatFramePass.h>
#include <render/passes/gaussian/GaussianSplatGraph.h>
#include <render/pipeline/FrameGraphPassNames.h>

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
        GaussianSplatRenderTarget renderTarget)
    {
        std::vector<GaussianSplatGraphHandles> handles;
        if (!ctx.gaussian)
            return handles;

        for (const GaussianSplatGraphResources& resources :
            ctx.gaussian->prepareGraphResources(renderTarget))
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

    rg::PassHandle registerGaussianSplatRenderStages(
        FrameGraphContext ctx,
        GaussianSplatRenderTarget renderTarget,
        rg::TextureHandle colorTarget,
        rg::TextureHandle sceneDepth,
        const char* uploadPassName,
        const char* sortPassName,
        const char* rasterPassName,
        const char* encodePassName,
        const char* decodePassName)
    {
        const std::vector<GaussianSplatGraphHandles> resources =
            importGaussianSplatGraphResources(ctx, renderTarget);
        if (resources.empty())
            return {};
        GaussianSplatFramePass* const gaussian = ctx.gaussian;

        const rg::PassHandle uploadPass = ctx.graph->addPass(
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
            [gaussian, renderTarget](rg::RenderPassContext& passCtx) {
                gaussian->executeUpload(passCtx.commandList(), renderTarget);
            },
            rg::PassOptions{ .sideEffect = true });

        const rg::PassHandle sortPass = ctx.graph->addPass(
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
            [gaussian](rg::RenderPassContext& passCtx) {
                gaussian->executeSort(passCtx.commandList());
            },
            rg::PassOptions{ .sideEffect = true, .after = uploadPass });

        const bool referenceGammaCompositing =
            renderTarget == GaussianSplatRenderTarget::ProcessedOutputColor
            && ctx.settings->GaussianSplatSortingMode == 0
            && ctx.settings->GaussianSplatReferenceGammaCompositing;
        rg::PassHandle rasterPredecessor = sortPass;
        if (referenceGammaCompositing)
        {
            rasterPredecessor = ctx.graph->addPass(
                encodePassName,
                [colorTarget](rg::PassBuilder& setup) {
                    setup.read(colorTarget, rg::TextureAccess::UnorderedAccess);
                    setup.write(colorTarget, rg::TextureAccess::UnorderedAccess);
                },
                [gaussian](rg::RenderPassContext& passCtx) {
                    gaussian->executeColorSpaceConversion(passCtx.commandList(), false);
                },
                rg::PassOptions{ .sideEffect = true, .after = sortPass });
        }

        const rg::PassHandle rasterPass = ctx.graph->addPass(
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
            [gaussian, renderTarget](rg::RenderPassContext& passCtx) {
                gaussian->executeRaster(passCtx.commandList(), renderTarget);
            },
            rg::PassOptions{ .sideEffect = true, .after = rasterPredecessor });

        rg::PassHandle ready = rasterPass;
        if (referenceGammaCompositing)
        {
            ready = ctx.graph->addPass(
                decodePassName,
                [colorTarget](rg::PassBuilder& setup) {
                    setup.read(colorTarget, rg::TextureAccess::UnorderedAccess);
                    setup.write(colorTarget, rg::TextureAccess::UnorderedAccess);
                },
                [gaussian](rg::RenderPassContext& passCtx) {
                    gaussian->executeColorSpaceConversion(passCtx.commandList(), true);
                },
                rg::PassOptions{ .sideEffect = true, .after = rasterPass });
        }
        return ready;
    }
}

rg::PassHandle registerGaussianSplatPreAAPass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.gaussian);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!needsTemporalGaussianSplatsBeforeAA(*ctx.settings))
        return {};

    RenderTargets& targets = *ctx.renderTargets;

    const rg::TextureHandle outputColor = ctx.graph->importTexture(
        targets.outputColor,
        rg::TextureAccess::UnorderedAccess);
    const rg::TextureHandle depth = ctx.graph->importTexture(
        targets.depth,
        rg::TextureAccess::ShaderResource);

    return registerGaussianSplatRenderStages(
        ctx,
        GaussianSplatRenderTarget::OutputColor,
        outputColor,
        depth,
        "GaussianSplatsStochasticUpload",
        "GaussianSplatsStochasticSort",
        "GaussianSplatsStochastic",
        "GaussianSplatsStochasticLinearToSrgb",
        "GaussianSplatsStochasticSrgbToLinear");
}

rg::PassHandle registerGaussianSplatAccelBuildPass(FrameGraphContext ctx, rg::PassHandle after)
{
    assert(ctx.graph);
    assert(ctx.gaussian);
    assert(ctx.settings);
    assert(after.isValid());

    if (!needsGaussianSplatAccelBuild(*ctx.settings))
        return after;

    // AABB buffers are the AS build inputs. The explicit edge still carries MainPathTrace
    // readiness (LightingEnd / VBuffer) until TLAS/BLAS are first-class graph resources.
    struct GaussianSplatAccelGraphHandles
    {
        rg::BufferHandle splatBuffer;
        rg::BufferHandle aabbBuffer;
    };
    std::vector<GaussianSplatAccelGraphHandles> accelResources;
    for (const GaussianSplatGraphResources& resources :
        ctx.gaussian->prepareGraphResources(GaussianSplatRenderTarget::OutputColor))
    {
        if (resources.splatBuffer == nullptr || resources.splatAabbBuffer == nullptr)
        {
            continue;
        }
        accelResources.push_back(GaussianSplatAccelGraphHandles{
            .splatBuffer = ctx.graph->importBuffer(resources.splatBuffer, rg::BufferAccess::CopyDest),
            .aabbBuffer = ctx.graph->importBuffer(
                resources.splatAabbBuffer,
                rg::BufferAccess::AccelStructBuildInput),
        });
    }

    rg::PassOptions passOptions{};
    // AS handles are not graph resources yet; sideEffect keeps the build alive for PT bindings.
    passOptions.sideEffect = true;
    passOptions.after = after;
    GaussianSplatFramePass* const gaussian = ctx.gaussian;

    return ctx.graph->addPass(
        kGaussianSplatsAccelBuildPass,
        [accelResources](rg::PassBuilder& setup) {
            for (const GaussianSplatAccelGraphHandles& item : accelResources)
            {
                setup.write(item.splatBuffer, rg::BufferAccess::CopyDest);
                setup.write(item.aabbBuffer, rg::BufferAccess::AccelStructBuildInput);
            }
        },
        [gaussian](rg::RenderPassContext& passCtx) {
            gaussian->executeAccelBuild(passCtx.commandList());
        },
        passOptions);
}

rg::PassHandle registerGaussianSplatCompositePass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.gaussian);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!needsGaussianSplatsCompositePass(*ctx.settings))
        return {};

    RenderTargets& targets = *ctx.renderTargets;

    const GaussianSplatRenderTarget renderTarget = ctx.settings->GaussianSplatApplyToneMapping
        ? GaussianSplatRenderTarget::ProcessedOutputColor
        : GaussianSplatRenderTarget::LdrColor;
    const rg::TextureHandle colorTarget = ctx.graph->importTexture(
        renderTarget == GaussianSplatRenderTarget::LdrColor ? targets.ldrColor : targets.processedOutputColor,
        renderTarget == GaussianSplatRenderTarget::LdrColor
            ? rg::TextureAccess::RenderTarget
            : rg::TextureAccess::UnorderedAccess);
    const rg::TextureHandle depth = ctx.graph->importTexture(
        targets.depth,
        rg::TextureAccess::ShaderResource);

    const rg::PassHandle compositeReady = registerGaussianSplatRenderStages(
        ctx,
        renderTarget,
        colorTarget,
        depth,
        "GaussianSplatsCompositeUpload",
        "GaussianSplatsCompositeSort",
        "GaussianSplatsComposite",
        "GaussianSplatsCompositeLinearToSrgb",
        "GaussianSplatsCompositeSrgbToLinear");
    if (!compositeReady.isValid())
        return {};

    if (!needsGaussianSplatTemporalAccumulate(*ctx.settings))
        return compositeReady;

    caustica::rhi::Texture* currentColorTexture = ctx.gaussian->currentColor();
    caustica::rhi::Texture* accumulatedColorTexture = ctx.gaussian->accumulatedColor();
    if (currentColorTexture == nullptr || accumulatedColorTexture == nullptr)
        return compositeReady;

    const rg::TextureHandle currentColor = ctx.graph->importTexture(
        currentColorTexture,
        rg::TextureAccess::ShaderResource);
    const rg::TextureHandle accumulatedColor = ctx.graph->importTexture(
        accumulatedColorTexture,
        rg::TextureAccess::UnorderedAccess);
    GaussianSplatFramePass* const gaussian = ctx.gaussian;

    const rg::PassHandle copyCurrentPass = ctx.graph->addPass(
        "GaussianSplatsCopyCurrent",
        [colorTarget, currentColor](rg::PassBuilder& setup) {
            setup.read(colorTarget, rg::TextureAccess::CopySource);
            setup.write(currentColor, rg::TextureAccess::CopyDest);
        },
        [colorTarget, currentColor](rg::RenderPassContext& passCtx) {
            passCtx.commandList()->copyTexture(
                passCtx.texture(currentColor), caustica::rhi::TextureSlice(),
                passCtx.texture(colorTarget), caustica::rhi::TextureSlice());
        },
        rg::PassOptions{
            .sideEffect = true,
            .after = compositeReady,
        });

    return ctx.graph->addPass(
        "GaussianSplatsAccumulate",
        [colorTarget, currentColor, accumulatedColor](rg::PassBuilder& setup) {
            setup.read(currentColor, rg::TextureAccess::ShaderResource);
            setup.read(accumulatedColor, rg::TextureAccess::UnorderedAccess);
            setup.write(accumulatedColor, rg::TextureAccess::UnorderedAccess);
            setup.write(colorTarget, rg::TextureAccess::UnorderedAccess);
        },
        [gaussian](rg::RenderPassContext& passCtx) {
            gaussian->executeAccumulate(passCtx.commandList());
        },
        rg::PassOptions{
            .sideEffect = true,
            .after = copyCurrentPass,
        });
}

} // namespace caustica::render

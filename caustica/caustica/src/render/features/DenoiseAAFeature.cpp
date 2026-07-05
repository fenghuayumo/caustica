#include <render/features/RenderFeature.h>

#include <render/core/CameraController.h>
#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/features/RenderFeatureContext.h>
#include <render/passes/postProcess/AccumulationPass.h>
#include <render/passes/geometry/TemporalAntiAliasingPass.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <scene/View.h>

#include <algorithm>
#include <cassert>

namespace caustica::render
{

namespace
{
    bool needsRealtimeCopyPass(const PathTracerSettings& settings)
    {
        return settings.RealtimeMode && settings.RealtimeAA == 0;
    }

    bool needsTemporalAAPass(const PathTracerSettings& settings, const TemporalAntiAliasingPass* taaPass)
    {
        return settings.RealtimeMode && settings.RealtimeAA == 1 && taaPass != nullptr;
    }

    bool needsAccumulationPass(const PathTracerSettings& settings, const AccumulationPass* accumulationPass)
    {
        return !settings.RealtimeMode && accumulationPass != nullptr;
    }

    bool needsDlssPass(const PathTracerSettings& settings)
    {
        return settings.RealtimeMode && (settings.RealtimeAA == 2 || settings.RealtimeAA == 3);
    }

    bool needsNoDenoiserFinalMergePass(const PathTracerSettings& settings)
    {
        return settings.RealtimeMode
            && !settings.ActualUseStandaloneDenoiser()
            && settings.RealtimeAA != 2
            && settings.RealtimeAA != 3;
    }

    bool needsStochasticGaussianSplatsBeforeAA(const PathTracerSettings& settings)
    {
        const bool stochasticSplats = settings.EnableGaussianSplats && settings.GaussianSplatSortingMode == 1;
        return stochasticSplats && settings.RealtimeMode && settings.RealtimeAA == 1;
    }

    bool needsGaussianSplatsCompositePass(const PathTracerSettings& settings)
    {
        if (!settings.EnableGaussianSplats)
            return false;

        const bool stochasticSplats = settings.GaussianSplatSortingMode == 1;
        const bool stochasticUsesMainTemporal = stochasticSplats
            && (!settings.RealtimeMode || settings.RealtimeAA == 1);
        return !stochasticUsesMainTemporal;
    }

    TemporalAntiAliasingParameters makeTemporalAAParameters(const RenderFeatureContext& ctx)
    {
        TemporalAntiAliasingParameters taaParams = ctx.settings->TemporalAntiAliasingParams;

        const bool stochasticSplats = ctx.settings->EnableGaussianSplats && ctx.settings->GaussianSplatSortingMode == 1;
        if (stochasticSplats && ctx.gaussianSplatTemporalSampleIndex != nullptr)
        {
            taaParams.enableHistoryClamping = false;
            taaParams.useHistoryClampRelax = false;
            taaParams.newFrameWeight = 1.0f / float(*ctx.gaussianSplatTemporalSampleIndex + 1);
        }

        return taaParams;
    }

    bool computeTemporalFeedbackValid(const RenderFeatureContext& ctx)
    {
        const bool stochasticSplats = ctx.settings->EnableGaussianSplats && ctx.settings->GaussianSplatSortingMode == 1;
        const bool stochasticReset = stochasticSplats && ctx.gaussianSplatTemporalReset != nullptr
            && (ctx.aaReset || ctx.settings->ResetAccumulation || ctx.settings->ResetRealtimeCaches
                || *ctx.gaussianSplatTemporalReset);
        return !ctx.aaReset && !stochasticReset && ctx.renderer->getFrameIndex() != 0;
    }

    void prepareDenoiseAAState(RenderFeatureContext& ctx)
    {
        if (!ctx.settings->RealtimeMode || ctx.settings->RealtimeAA != 1)
            return;

        const bool stochasticSplats = ctx.settings->EnableGaussianSplats && ctx.settings->GaussianSplatSortingMode == 1;
        if (!stochasticSplats || ctx.gaussianSplatTemporalReset == nullptr || ctx.gaussianSplatTemporalSampleIndex == nullptr)
            return;

        const bool stochasticReset = ctx.aaReset || ctx.settings->ResetAccumulation || ctx.settings->ResetRealtimeCaches
            || *ctx.gaussianSplatTemporalReset;
        if (stochasticReset)
            *ctx.gaussianSplatTemporalSampleIndex = 0;
    }
}

void registerDenoiseAAFeature(RenderFeatureContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    prepareDenoiseAAState(ctx);

    RenderTargets& targets = *ctx.renderTargets;

    const rg::TextureHandle outputColor = ctx.graph->importTexture(
        targets.outputColor,
        rg::TextureAccess::UnorderedAccess);
    const rg::TextureHandle processedOutputColor = ctx.graph->importTexture(
        targets.processedOutputColor,
        rg::TextureAccess::UnorderedAccess);
    const rg::TextureHandle accumulatedRadiance = ctx.graph->importTexture(
        targets.accumulatedRadiance,
        rg::TextureAccess::UnorderedAccess);
    const rg::TextureHandle motionVectors = ctx.graph->importTexture(
        targets.screenMotionVectors,
        rg::TextureAccess::ShaderResource);
    const rg::TextureHandle temporalFeedback1 = ctx.graph->importTexture(
        targets.temporalFeedback1,
        rg::TextureAccess::UnorderedAccess);
    const rg::TextureHandle temporalFeedback2 = ctx.graph->importTexture(
        targets.temporalFeedback2,
        rg::TextureAccess::UnorderedAccess);
    const rg::TextureHandle historyClampRelax = ctx.graph->importTexture(
        targets.combinedHistoryClampRelax,
        rg::TextureAccess::ShaderResource);

    ctx.graph->extractTexture(outputColor, rg::TextureAccess::UnorderedAccess);
    ctx.graph->extractTexture(processedOutputColor, rg::TextureAccess::UnorderedAccess);
    ctx.graph->extractTexture(accumulatedRadiance, rg::TextureAccess::UnorderedAccess);
    ctx.graph->extractTexture(temporalFeedback1, rg::TextureAccess::UnorderedAccess);
    ctx.graph->extractTexture(temporalFeedback2, rg::TextureAccess::UnorderedAccess);

    TemporalAntiAliasingPass* temporalAAPass = ctx.renderer->getTemporalAntiAliasingPass();
    AccumulationPass* accumulationPass = ctx.renderer->getAccumulationPass();

    if (needsNoDenoiserFinalMergePass(*ctx.settings))
    {
        const rg::TextureHandle stableRadiance = ctx.graph->importTexture(
            targets.stableRadiance,
            rg::TextureAccess::ShaderResource);
        const rg::TextureHandle stablePlanesHeader = ctx.graph->importTexture(
            targets.stablePlanesHeader,
            rg::TextureAccess::ShaderResource);
        const rg::BufferHandle stablePlanesBuffer = ctx.graph->importBuffer(
            targets.stablePlanesBuffer,
            rg::BufferAccess::ShaderResource);

        ctx.graph->addPass(
            "NoDenoiserFinalMerge",
            [&](rg::PassBuilder& setup) {
                setup.read(outputColor, rg::TextureAccess::UnorderedAccess);
                setup.read(stableRadiance, rg::TextureAccess::ShaderResource);
                setup.read(stablePlanesHeader, rg::TextureAccess::ShaderResource);
                setup.read(stablePlanesBuffer, rg::BufferAccess::ShaderResource);
                setup.write(outputColor, rg::TextureAccess::UnorderedAccess);
            },
            [ctx](rg::RenderPassContext& passCtx) {
                ctx.renderer->runNoDenoiserFinalMerge(passCtx.commandList());
            },
            rg::PassOptions{ .sideEffect = true });
    }

    if (needsRealtimeCopyPass(*ctx.settings))
    {
        ctx.graph->addPass(
            "CopyOutputToProcessed",
            [&](rg::PassBuilder& setup) {
                setup.read(outputColor, rg::TextureAccess::CopySource);
                setup.write(processedOutputColor, rg::TextureAccess::CopyDest);
            },
            [outputColor, processedOutputColor](rg::RenderPassContext& passCtx) {
                passCtx.commandList()->copyTexture(
                    passCtx.texture(processedOutputColor),
                    nvrhi::TextureSlice(),
                    passCtx.texture(outputColor),
                    nvrhi::TextureSlice());
            });
    }

    if (needsStochasticGaussianSplatsBeforeAA(*ctx.settings))
    {
        const rg::TextureHandle depth = ctx.graph->importTexture(
            targets.depth,
            rg::TextureAccess::ShaderResource);

        ctx.graph->addPass(
            "Gaussian Splats Stochastic",
            [&](rg::PassBuilder& setup) {
                setup.read(outputColor, rg::TextureAccess::UnorderedAccess);
                setup.read(depth, rg::TextureAccess::ShaderResource);
                setup.write(outputColor, rg::TextureAccess::UnorderedAccess);
            },
            [ctx](rg::RenderPassContext& passCtx) {
                ctx.renderer->renderGaussianSplats(passCtx.commandList(), true);
            },
            rg::PassOptions{ .sideEffect = true });
    }

    if (needsTemporalAAPass(*ctx.settings, temporalAAPass))
    {
        const auto taaParams = makeTemporalAAParameters(ctx);
        const bool feedbackIsValid = computeTemporalFeedbackValid(ctx);
        const bool stochasticSplats = ctx.settings->EnableGaussianSplats && ctx.settings->GaussianSplatSortingMode == 1;

        ctx.graph->addPass(
            "TAA",
            [&](rg::PassBuilder& setup) {
                setup.read(outputColor, rg::TextureAccess::ShaderResource);
                setup.read(motionVectors, rg::TextureAccess::ShaderResource);
                setup.read(temporalFeedback1, rg::TextureAccess::ShaderResource);
                setup.read(temporalFeedback2, rg::TextureAccess::UnorderedAccess);
                setup.read(historyClampRelax, rg::TextureAccess::ShaderResource);
                setup.write(processedOutputColor, rg::TextureAccess::UnorderedAccess);
                setup.write(temporalFeedback1, rg::TextureAccess::UnorderedAccess);
                setup.write(temporalFeedback2, rg::TextureAccess::UnorderedAccess);
            },
            [ctx, temporalAAPass, taaParams, feedbackIsValid, stochasticSplats](rg::RenderPassContext& passCtx) {
                const ICompositeView& taaView = *ctx.renderer->getCameraController().view();
                temporalAAPass->TemporalResolve(
                    passCtx.commandList(),
                    taaParams,
                    feedbackIsValid,
                    taaView,
                    taaView);

                if (stochasticSplats && ctx.gaussianSplatTemporalSampleIndex != nullptr
                    && ctx.gaussianSplatTemporalReset != nullptr)
                {
                    *ctx.gaussianSplatTemporalSampleIndex =
                        std::min(*ctx.gaussianSplatTemporalSampleIndex + 1, 1024 * 1024);
                    *ctx.gaussianSplatTemporalReset = false;
                }
            });
    }

    if (needsDlssPass(*ctx.settings))
    {
        const bool dlssRayReconstruction = ctx.settings->RealtimeAA == 3;
        const rg::TextureHandle depth = ctx.graph->importTexture(
            targets.depth,
            rg::TextureAccess::ShaderResource);
        const rg::TextureHandle preUIColor = ctx.graph->importTexture(
            targets.preUIColor,
            rg::TextureAccess::ShaderResource);

        ctx.graph->addPass(
            dlssRayReconstruction ? "DLSS-RR" : "DLSS",
            [&](rg::PassBuilder& setup) {
                setup.read(outputColor, rg::TextureAccess::ShaderResource);
                setup.read(motionVectors, rg::TextureAccess::ShaderResource);
                setup.read(depth, rg::TextureAccess::ShaderResource);
                setup.read(preUIColor, rg::TextureAccess::ShaderResource);
                setup.write(processedOutputColor, rg::TextureAccess::UnorderedAccess);

                if (dlssRayReconstruction)
                {
                    const rg::TextureHandle rrDiffuseAlbedo = ctx.graph->importTexture(
                        targets.rrDiffuseAlbedo,
                        rg::TextureAccess::ShaderResource);
                    const rg::TextureHandle rrSpecAlbedo = ctx.graph->importTexture(
                        targets.rrSpecAlbedo,
                        rg::TextureAccess::ShaderResource);
                    const rg::TextureHandle rrNormalsAndRoughness = ctx.graph->importTexture(
                        targets.rrNormalsAndRoughness,
                        rg::TextureAccess::ShaderResource);
                    setup.read(rrDiffuseAlbedo, rg::TextureAccess::ShaderResource);
                    setup.read(rrSpecAlbedo, rg::TextureAccess::ShaderResource);
                    setup.read(rrNormalsAndRoughness, rg::TextureAccess::ShaderResource);
                }
            },
            [ctx](rg::RenderPassContext& passCtx) {
                ctx.renderer->runDlssUpscale(passCtx.commandList(), ctx.aaReset);
            },
            rg::PassOptions{ .sideEffect = true });
    }

    if (needsAccumulationPass(*ctx.settings, accumulationPass))
    {
        const int accumulationSampleIndex = ctx.renderer->getAccumulationSampleIndex();
        const float accumulationWeight = (accumulationSampleIndex < ctx.settings->AccumulationTarget)
            ? (1.f / float(std::max(0, accumulationSampleIndex) + 1))
            : 0.0f;

        ctx.graph->addPass(
            "Accumulation",
            [&](rg::PassBuilder& setup) {
                setup.read(outputColor, rg::TextureAccess::ShaderResource);
                setup.write(accumulatedRadiance, rg::TextureAccess::UnorderedAccess);
                setup.write(processedOutputColor, rg::TextureAccess::UnorderedAccess);
            },
            [ctx, accumulationPass, accumulationWeight](rg::RenderPassContext& passCtx) {
                const IView& view = *ctx.renderer->getCameraController().view();
                accumulationPass->Render(
                    passCtx.commandList(),
                    view,
                    view,
                    accumulationWeight);
            });
    }

    if (needsGaussianSplatsCompositePass(*ctx.settings))
    {
        const rg::TextureHandle depth = ctx.graph->importTexture(
            targets.depth,
            rg::TextureAccess::ShaderResource);

        ctx.graph->addPass(
            "Gaussian Splats",
            [&](rg::PassBuilder& setup) {
                setup.read(processedOutputColor, rg::TextureAccess::UnorderedAccess);
                setup.read(depth, rg::TextureAccess::ShaderResource);
                setup.write(processedOutputColor, rg::TextureAccess::UnorderedAccess);
            },
            [ctx](rg::RenderPassContext& passCtx) {
                ctx.renderer->renderGaussianSplats(passCtx.commandList(), false);
            },
            rg::PassOptions{ .sideEffect = true });
    }
}

} // namespace caustica::render

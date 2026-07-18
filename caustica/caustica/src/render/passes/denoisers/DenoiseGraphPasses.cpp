#include <render/FrameGraphPasses.h>

#include <render/FrameGraphContext.h>
#include <render/core/CameraController.h>
#include <render/core/PathTracerSettings.h>
#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/denoisers/DenoisePass.h>
#include <render/passes/geometry/TemporalAntiAliasingPass.h>
#include <render/passes/pathTrace/PathTraceGraphResources.h>
#include <render/passes/postProcess/AccumulationPass.h>
#include <scene/View.h>
#include <shaders/PathTracer/Config.h>

#include <algorithm>
#include <cassert>
#include <format>

namespace caustica::render
{

namespace
{
    bool needsStablePlanesDebugViz(const PathTracerSettings& settings)
    {
        if (!settings.RealtimeMode)
            return false;

        return (settings.DebugView > DebugViewType::Disabled
                && settings.DebugView <= DebugViewType::StablePlane_SpecRadiance)
            || settings.DebugView == DebugViewType::StableRadiance;
    }
}

void registerDenoiserPreparePass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.denoise);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!ctx.hasScene)
        return;

    const PathTraceGraphTargets handles = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);
    extractPathTraceGraphOutputs(*ctx.graph, handles);

    rg::PassOptions denoiserPassOptions{};
    denoiserPassOptions.executeAfter = ctx.settings->actualUseRTXDIPasses() ? "Rtxdi" : "MainPathTrace";

    ctx.graph->addPass(
        "DenoiserPrepare",
        [handles](rg::PassBuilder& setup) {
            declareDenoiserPrepareAccess(setup, handles);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            ctx.denoise->prepareGuides(passCtx.commandList());
        },
        denoiserPassOptions);

    if (needsStablePlanesDebugViz(*ctx.settings))
    {
        rg::PassOptions debugVizPassOptions{};
        debugVizPassOptions.executeAfter = "DenoiserPrepare";

        ctx.graph->addPass(
            "StablePlanesDebugViz",
            [handles](rg::PassBuilder& setup) {
                declareStablePlanesDebugVizAccess(setup, handles);
            },
            [ctx](rg::RenderPassContext& passCtx) {
                ctx.denoise->stablePlanesDebugViz(passCtx.commandList());
            },
            debugVizPassOptions);
    }
}

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

void registerNrdPass(FrameGraphContext ctx)
{
    assert(ctx.targetFramebuffer);
    assert(ctx.denoise);
    assert(ctx.renderTargets);
    assert(ctx.settings);
    assert(ctx.graph);

    if (!ctx.hasScene || !ctx.settings->actualUseStandaloneDenoiser())
        return;

    ctx.denoise->ensureNrdIntegrations();

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
                ctx.denoise->denoiseStablePlane(
                    passCtx.commandList(),
                    ctx.targetFramebuffer,
                    planeIndex);
            });
    }
}

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
            && !settings.actualUseStandaloneDenoiser()
            && settings.RealtimeAA != 2
            && settings.RealtimeAA != 3;
    }

    TemporalAntiAliasingParameters makeTemporalAAParameters(const FrameGraphContext& ctx)
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

    bool computeTemporalFeedbackValid(const FrameGraphContext& ctx)
    {
        const bool stochasticSplats = ctx.settings->EnableGaussianSplats && ctx.settings->GaussianSplatSortingMode == 1;
        const bool stochasticReset = stochasticSplats && ctx.gaussianSplatTemporalReset != nullptr
            && (ctx.aaReset || ctx.settings->ResetAccumulation || ctx.settings->ResetRealtimeCaches
                || *ctx.gaussianSplatTemporalReset);
        return !ctx.aaReset && !stochasticReset && ctx.frameIndex != 0;
    }

    void prepareDenoiseAAState(FrameGraphContext& ctx)
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

void registerDenoiseAAPass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.denoise);
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

    TemporalAntiAliasingPass* temporalAAPass = ctx.temporalAntiAliasing;
    AccumulationPass* accumulationPass = ctx.accumulation;

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
                ctx.denoise->runNoDenoiserFinalMerge(passCtx.commandList());
            },
            rg::PassOptions{ .sideEffect = true });
    }

    if (needsRealtimeCopyPass(*ctx.settings))
    {
        // RealtimeAA==0 should keep renderSize == displaySize; refuse a mismatched
        // copy that would leave the rest of processedOutputColor uninitialized.
        const bool sizesMatch =
            ctx.renderSize.x == ctx.displaySize.x && ctx.renderSize.y == ctx.displaySize.y;
        if (sizesMatch)
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
                const ICompositeView& taaView = *ctx.compositeView;
                temporalAAPass->temporalResolve(
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
                ctx.denoise->runDlssUpscale(passCtx.commandList(), ctx.aaReset);
            },
            rg::PassOptions{ .sideEffect = true });
    }

    if (needsAccumulationPass(*ctx.settings, accumulationPass))
    {
        const int accumulationSampleIndex = ctx.accumulationSampleIndex;
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
                const IView& view = *ctx.view;
                accumulationPass->render(
                    passCtx.commandList(),
                    view,
                    view,
                    accumulationWeight);
            });
    }
}

void registerDenoiseGraphPasses(FrameGraphContext ctx)
{
    registerDenoiserPreparePass(ctx);
    registerNrdPass(ctx);
    registerDenoiseAAPass(ctx);
}

} // namespace caustica::render

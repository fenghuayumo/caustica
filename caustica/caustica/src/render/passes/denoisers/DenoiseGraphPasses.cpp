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
#include <render/pipeline/FrameGraphPassNames.h>
#include <scene/View.h>
#include <shaders/PathTracer/Config.h>
#include <shaders/FrameConstantBuffer.h>

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

rg::PassHandle registerDenoiserPreparePass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.denoise);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!ctx.hasScene)
        return {};

    const PathTraceGraphTargets handles = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);
    DenoisePass* const denoise = ctx.denoise;

    ctx.graph->addPass(
        kDenoiseSpecHitTPass,
        [handles](rg::PassBuilder& setup) {
            setup.write(handles.depth, rg::TextureAccess::UnorderedAccess);
            setup.write(handles.specularHitT, rg::TextureAccess::UnorderedAccess);
            setup.write(handles.scratchFloat1, rg::TextureAccess::UnorderedAccess);
        },
        [denoise](rg::RenderPassContext& passCtx) {
            denoise->denoiseSpecHitT(passCtx.commandList());
        },
        rg::PassOptions{ .queue = caustica::rhi::CommandQueue::Compute });

    rg::PassHandle guidesReady = ctx.graph->addPass(
        kAvgLayerRadiancePass,
        [handles](rg::PassBuilder& setup) {
            declareDenoiserPrepareAccess(setup, handles);
        },
        [denoise](rg::RenderPassContext& passCtx) {
            denoise->computeAvgLayerRadiance(passCtx.commandList());
        },
        rg::PassOptions{ .queue = caustica::rhi::CommandQueue::Compute });

    if (needsStablePlanesDebugViz(*ctx.settings))
    {
        guidesReady = ctx.graph->addPass(
            kStablePlanesDebugVizPass,
            [handles](rg::PassBuilder& setup) {
                declareStablePlanesDebugVizAccess(setup, handles);
            },
            [denoise](rg::RenderPassContext& passCtx) {
                denoise->stablePlanesDebugViz(passCtx.commandList());
            });
    }

    return guidesReady;
}

namespace
{
    struct NrdPlaneGraphHandles
    {
        rg::TextureHandle denoiserViewspaceZ;
        rg::TextureHandle denoiserMotionVectors;
        rg::TextureHandle denoiserNormalRoughness;
        rg::TextureHandle denoiserDiffRadianceHitDist;
        rg::TextureHandle denoiserSpecRadianceHitDist;
        rg::TextureHandle denoiserDisocclusionThresholdMix;
        rg::TextureHandle historyClampRelax;
        rg::TextureHandle outputColor;
        rg::TextureHandle outDiff;
        rg::TextureHandle outSpec;
        rg::TextureHandle validation;
        rg::TextureHandle stableRadiance;
        rg::TextureHandle stablePlanesHeader;
        rg::BufferHandle stablePlanesBuffer;
        rg::TextureHandle specularHitT;
    };

    NrdPlaneGraphHandles importNrdPlaneHandles(
        rg::GraphBuilder& graph,
        RenderTargets& targets,
        int planeIndex)
    {
        NrdPlaneGraphHandles handles{};
        handles.denoiserViewspaceZ = graph.importTexture(
            targets.denoiserViewspaceZ, rg::TextureAccess::UnorderedAccess);
        handles.denoiserMotionVectors = graph.importTexture(
            targets.denoiserMotionVectors, rg::TextureAccess::UnorderedAccess);
        handles.denoiserNormalRoughness = graph.importTexture(
            targets.denoiserNormalRoughness, rg::TextureAccess::UnorderedAccess);
        handles.denoiserDiffRadianceHitDist = graph.importTexture(
            targets.denoiserDiffRadianceHitDist, rg::TextureAccess::UnorderedAccess);
        handles.denoiserSpecRadianceHitDist = graph.importTexture(
            targets.denoiserSpecRadianceHitDist, rg::TextureAccess::UnorderedAccess);
        handles.denoiserDisocclusionThresholdMix = graph.importTexture(
            targets.denoiserDisocclusionThresholdMix, rg::TextureAccess::UnorderedAccess);
        handles.historyClampRelax = graph.importTexture(
            targets.combinedHistoryClampRelax, rg::TextureAccess::UnorderedAccess);
        handles.outputColor = graph.importTexture(
            targets.outputColor, rg::TextureAccess::UnorderedAccess);
        handles.outDiff = graph.importTexture(
            targets.denoiserOutDiffRadianceHitDist[planeIndex], rg::TextureAccess::UnorderedAccess);
        handles.outSpec = graph.importTexture(
            targets.denoiserOutSpecRadianceHitDist[planeIndex], rg::TextureAccess::UnorderedAccess);
        if (targets.denoiserOutValidation != nullptr)
        {
            handles.validation = graph.importTexture(
                targets.denoiserOutValidation, rg::TextureAccess::UnorderedAccess);
        }
        handles.stableRadiance = graph.importTexture(
            targets.stableRadiance, rg::TextureAccess::ShaderResource);
        handles.stablePlanesHeader = graph.importTexture(
            targets.stablePlanesHeader, rg::TextureAccess::ShaderResource);
        handles.stablePlanesBuffer = graph.importBuffer(
            targets.stablePlanesBuffer, rg::BufferAccess::ShaderResource);
        handles.specularHitT = graph.importTexture(
            targets.specularHitT, rg::TextureAccess::ShaderResource);

        return handles;
    }

    void declareNrdPrepareAccess(
        rg::PassBuilder& setup,
        const NrdPlaneGraphHandles& handles,
        bool initWithStableRadiance)
    {
        setup.read(handles.stableRadiance, rg::TextureAccess::ShaderResource);
        setup.read(handles.stablePlanesHeader, rg::TextureAccess::ShaderResource);
        setup.read(handles.stablePlanesBuffer, rg::BufferAccess::ShaderResource);
        setup.read(handles.specularHitT, rg::TextureAccess::ShaderResource);

        setup.write(handles.denoiserViewspaceZ, rg::TextureAccess::UnorderedAccess);
        setup.write(handles.denoiserMotionVectors, rg::TextureAccess::UnorderedAccess);
        setup.write(handles.denoiserNormalRoughness, rg::TextureAccess::UnorderedAccess);
        setup.write(handles.denoiserDiffRadianceHitDist, rg::TextureAccess::UnorderedAccess);
        setup.write(handles.denoiserSpecRadianceHitDist, rg::TextureAccess::UnorderedAccess);
        setup.write(handles.denoiserDisocclusionThresholdMix, rg::TextureAccess::UnorderedAccess);
        setup.write(handles.historyClampRelax, rg::TextureAccess::UnorderedAccess);
        if (initWithStableRadiance)
            setup.write(handles.outputColor, rg::TextureAccess::UnorderedAccess);
    }

    void declareNrdRunAccess(rg::PassBuilder& setup, const NrdPlaneGraphHandles& handles)
    {
        setup.read(handles.denoiserViewspaceZ, rg::TextureAccess::ShaderResource);
        setup.read(handles.denoiserMotionVectors, rg::TextureAccess::ShaderResource);
        setup.read(handles.denoiserNormalRoughness, rg::TextureAccess::ShaderResource);
        setup.read(handles.denoiserDiffRadianceHitDist, rg::TextureAccess::ShaderResource);
        setup.read(handles.denoiserSpecRadianceHitDist, rg::TextureAccess::ShaderResource);
        setup.read(handles.denoiserDisocclusionThresholdMix, rg::TextureAccess::ShaderResource);
        setup.write(handles.outDiff, rg::TextureAccess::UnorderedAccess);
        setup.write(handles.outSpec, rg::TextureAccess::UnorderedAccess);
        if (handles.validation.isValid())
            setup.write(handles.validation, rg::TextureAccess::UnorderedAccess);
    }

    void rebaseNrdPlaneHandles(rg::GraphBuilder& graph, NrdPlaneGraphHandles& handles)
    {
        handles.denoiserViewspaceZ = graph.currentTexture(handles.denoiserViewspaceZ);
        handles.denoiserMotionVectors = graph.currentTexture(handles.denoiserMotionVectors);
        handles.denoiserNormalRoughness = graph.currentTexture(handles.denoiserNormalRoughness);
        handles.denoiserDiffRadianceHitDist = graph.currentTexture(handles.denoiserDiffRadianceHitDist);
        handles.denoiserSpecRadianceHitDist = graph.currentTexture(handles.denoiserSpecRadianceHitDist);
        handles.denoiserDisocclusionThresholdMix =
            graph.currentTexture(handles.denoiserDisocclusionThresholdMix);
        handles.historyClampRelax = graph.currentTexture(handles.historyClampRelax);
        handles.outputColor = graph.currentTexture(handles.outputColor);
        handles.outDiff = graph.currentTexture(handles.outDiff);
        handles.outSpec = graph.currentTexture(handles.outSpec);
        if (handles.validation.isValid())
            handles.validation = graph.currentTexture(handles.validation);
    }

    void declareNrdMergeAccess(
        rg::PassBuilder& setup,
        const NrdPlaneGraphHandles& handles,
        bool readsExistingOutputColor)
    {
        setup.read(handles.outDiff, rg::TextureAccess::ShaderResource);
        setup.read(handles.outSpec, rg::TextureAccess::ShaderResource);
        setup.read(handles.denoiserViewspaceZ, rg::TextureAccess::ShaderResource);
        setup.read(handles.denoiserDisocclusionThresholdMix, rg::TextureAccess::ShaderResource);
        setup.read(handles.stablePlanesBuffer, rg::BufferAccess::ShaderResource);
        if (handles.validation.isValid())
            setup.read(handles.validation, rg::TextureAccess::ShaderResource);
        if (readsExistingOutputColor)
            setup.read(handles.outputColor, rg::TextureAccess::UnorderedAccess);
        setup.write(handles.outputColor, rg::TextureAccess::UnorderedAccess);
    }
}

rg::PassHandle registerNrdPass(FrameGraphContext ctx)
{
    assert(ctx.denoise);
    assert(ctx.renderTargets);
    assert(ctx.settings);
    assert(ctx.graph);

    if (!ctx.hasScene || !ctx.settings->actualUseStandaloneDenoiser())
        return {};

    ctx.denoise->ensureNrdIntegrations();
    DenoisePass* const denoise = ctx.denoise;

    const int maxPassCount = std::min(
        ctx.settings->StablePlanesActiveCount,
        static_cast<int>(cStablePlaneCount));

    // Highest plane first (initializes outputColor from stable radiance), then lower planes
    // accumulate. Resource edges (specularHitT / outDiff / outputColor) order Prepare → Run → Merge.
    rg::PassHandle nrdReady{};

    for (int pass = maxPassCount - 1; pass >= 0; --pass)
    {
        const int planeIndex = pass;
        assert(planeIndex >= 0 && planeIndex < static_cast<int>(cStablePlaneCount));

        const bool initWithStableRadiance = planeIndex == (maxPassCount - 1);
        const bool readsExistingOutputColor = planeIndex < (maxPassCount - 1);
        NrdPlaneGraphHandles handles = importNrdPlaneHandles(
            *ctx.graph,
            *ctx.renderTargets,
            planeIndex);

        ctx.graph->addPass(
            nrdPreparePassName(planeIndex),
            [handles, initWithStableRadiance](rg::PassBuilder& setup) {
                declareNrdPrepareAccess(setup, handles, initWithStableRadiance);
            },
            [denoise, planeIndex](rg::RenderPassContext& passCtx) {
                denoise->prepareNrdInputs(passCtx.commandList(), planeIndex);
            });
        rebaseNrdPlaneHandles(*ctx.graph, handles);

        ctx.graph->addPass(
            nrdRunPassName(planeIndex),
            [handles](rg::PassBuilder& setup) {
                declareNrdRunAccess(setup, handles);
            },
            [denoise, planeIndex](rg::RenderPassContext& passCtx) {
                denoise->runNrd(passCtx.commandList(), planeIndex);
            },
            rg::PassOptions{ .queue = caustica::rhi::CommandQueue::Compute });
        rebaseNrdPlaneHandles(*ctx.graph, handles);

        nrdReady = ctx.graph->addPass(
            nrdMergePassName(planeIndex),
            [handles, readsExistingOutputColor](rg::PassBuilder& setup) {
                declareNrdMergeAccess(setup, handles, readsExistingOutputColor);
            },
            [denoise, planeIndex](rg::RenderPassContext& passCtx) {
                denoise->mergeNrdOutputs(passCtx.commandList(), planeIndex);
            });
    }

    return nrdReady;
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

rg::PassHandle registerDenoiseAAPass(FrameGraphContext ctx, FrameSlots& slots)
{
    assert(ctx.graph);
    assert(ctx.denoise);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    prepareDenoiseAAState(ctx);

    RenderTargets& targets = *ctx.renderTargets;

    rg::TextureHandle outputColor = slots.outputColor;
    rg::TextureHandle processedOutputColor = slots.hdrColor;
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

    // History leaves the graph; current-frame color is consumed by later in-graph passes.
    ctx.graph->extractTexture(accumulatedRadiance, rg::TextureAccess::UnorderedAccess);
    ctx.graph->extractTexture(temporalFeedback1, rg::TextureAccess::UnorderedAccess);
    ctx.graph->extractTexture(temporalFeedback2, rg::TextureAccess::UnorderedAccess);

    TemporalAntiAliasingPass* temporalAAPass = ctx.temporalAntiAliasing;
    AccumulationPass* accumulationPass = ctx.accumulation;
    DenoisePass* const denoise = ctx.denoise;
    rg::PassHandle denoiseReady{};

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

        denoiseReady = ctx.graph->addPass(
            "NoDenoiserFinalMerge",
            [outputColor, stableRadiance, stablePlanesHeader, stablePlanesBuffer](rg::PassBuilder& setup) {
                setup.read(outputColor, rg::TextureAccess::UnorderedAccess);
                setup.read(stableRadiance, rg::TextureAccess::ShaderResource);
                setup.read(stablePlanesHeader, rg::TextureAccess::ShaderResource);
                setup.read(stablePlanesBuffer, rg::BufferAccess::ShaderResource);
                setup.write(outputColor, rg::TextureAccess::UnorderedAccess);
            },
            [denoise](rg::RenderPassContext& passCtx) {
                denoise->runNoDenoiserFinalMerge(passCtx.commandList());
            },
            rg::PassOptions{ .sideEffect = true });
        outputColor = ctx.graph->currentTexture(outputColor);
        processedOutputColor = ctx.graph->currentTexture(processedOutputColor);
    }

    if (needsRealtimeCopyPass(*ctx.settings))
    {
        // RealtimeAA==0 should keep renderSize == displaySize; refuse a mismatched
        // copy that would leave the rest of processedOutputColor uninitialized.
        const bool sizesMatch =
            ctx.renderSize.x == ctx.displaySize.x && ctx.renderSize.y == ctx.displaySize.y;
        if (sizesMatch)
        {
            denoiseReady = ctx.graph->addPass(
                "CopyOutputToProcessed",
                [outputColor, processedOutputColor](rg::PassBuilder& setup) {
                    setup.read(outputColor, rg::TextureAccess::CopySource);
                    setup.write(processedOutputColor, rg::TextureAccess::CopyDest);
                },
                [outputColor, processedOutputColor](rg::RenderPassContext& passCtx) {
                    passCtx.commandList()->copyTexture(
                        passCtx.texture(processedOutputColor),
                        caustica::rhi::TextureSlice(),
                        passCtx.texture(outputColor),
                        caustica::rhi::TextureSlice());
                });
            outputColor = ctx.graph->currentTexture(outputColor);
            processedOutputColor = ctx.graph->currentTexture(processedOutputColor);
        }
    }

    if (needsTemporalAAPass(*ctx.settings, temporalAAPass))
    {
        const auto taaParams = makeTemporalAAParameters(ctx);
        const bool feedbackIsValid = computeTemporalFeedbackValid(ctx);
        const bool stochasticSplats = ctx.settings->EnableGaussianSplats && ctx.settings->GaussianSplatSortingMode == 1;
        const ViewInfo* const view = ctx.view;
        int* const gaussianSampleIndex = ctx.gaussianSplatTemporalSampleIndex;
        bool* const gaussianTemporalReset = ctx.gaussianSplatTemporalReset;

        denoiseReady = ctx.graph->addPass(
            "TAA",
            [outputColor, motionVectors, temporalFeedback1, temporalFeedback2,
             historyClampRelax, processedOutputColor](rg::PassBuilder& setup) {
                setup.read(outputColor, rg::TextureAccess::ShaderResource);
                setup.read(motionVectors, rg::TextureAccess::ShaderResource);
                setup.read(temporalFeedback1, rg::TextureAccess::ShaderResource);
                setup.read(temporalFeedback2, rg::TextureAccess::UnorderedAccess);
                setup.read(historyClampRelax, rg::TextureAccess::ShaderResource);
                setup.write(processedOutputColor, rg::TextureAccess::UnorderedAccess);
                setup.write(temporalFeedback1, rg::TextureAccess::UnorderedAccess);
                setup.write(temporalFeedback2, rg::TextureAccess::UnorderedAccess);
            },
            [temporalAAPass, taaParams, feedbackIsValid, stochasticSplats, view,
             gaussianSampleIndex, gaussianTemporalReset](rg::RenderPassContext& passCtx) {
                const ViewInfo& taaView = *view;
                temporalAAPass->temporalResolve(
                    passCtx.commandList(),
                    taaParams,
                    feedbackIsValid,
                    taaView,
                    taaView);

                if (stochasticSplats && gaussianSampleIndex != nullptr
                    && gaussianTemporalReset != nullptr)
                {
                    *gaussianSampleIndex = std::min(*gaussianSampleIndex + 1, 1024 * 1024);
                    *gaussianTemporalReset = false;
                }
            });
        outputColor = ctx.graph->currentTexture(outputColor);
        processedOutputColor = ctx.graph->currentTexture(processedOutputColor);
    }

    if (needsDlssPass(*ctx.settings))
    {
        const bool dlssRayReconstruction = ctx.settings->RealtimeAA == 3;
        const rg::TextureHandle depth = ctx.graph->currentTexture(slots.depth);
        const rg::TextureHandle preUIColor = ctx.graph->importTexture(
            targets.preUIColor,
            rg::TextureAccess::ShaderResource);
        rg::TextureHandle rrDiffuseAlbedo{};
        rg::TextureHandle rrSpecAlbedo{};
        rg::TextureHandle rrNormalsAndRoughness{};
        if (dlssRayReconstruction)
        {
            rrDiffuseAlbedo = ctx.graph->importTexture(
                targets.rrDiffuseAlbedo, rg::TextureAccess::ShaderResource);
            rrSpecAlbedo = ctx.graph->importTexture(
                targets.rrSpecAlbedo, rg::TextureAccess::ShaderResource);
            rrNormalsAndRoughness = ctx.graph->importTexture(
                targets.rrNormalsAndRoughness, rg::TextureAccess::ShaderResource);
        }
        const bool aaReset = ctx.aaReset;

        denoiseReady = ctx.graph->addPass(
            dlssRayReconstruction ? "DLSS-RR" : "DLSS",
            [outputColor, motionVectors, depth, preUIColor, processedOutputColor,
             rrDiffuseAlbedo, rrSpecAlbedo, rrNormalsAndRoughness](rg::PassBuilder& setup) {
                setup.read(outputColor, rg::TextureAccess::ShaderResource);
                setup.read(motionVectors, rg::TextureAccess::ShaderResource);
                setup.read(depth, rg::TextureAccess::ShaderResource);
                setup.read(preUIColor, rg::TextureAccess::ShaderResource);
                setup.write(processedOutputColor, rg::TextureAccess::UnorderedAccess);

                if (rrDiffuseAlbedo.isValid())
                {
                    setup.read(rrDiffuseAlbedo, rg::TextureAccess::ShaderResource);
                    setup.read(rrSpecAlbedo, rg::TextureAccess::ShaderResource);
                    setup.read(rrNormalsAndRoughness, rg::TextureAccess::ShaderResource);
                }
            },
            [denoise, aaReset](rg::RenderPassContext& passCtx) {
                denoise->runDlssUpscale(passCtx.commandList(), aaReset);
            },
            rg::PassOptions{ .sideEffect = true });
        outputColor = ctx.graph->currentTexture(outputColor);
        processedOutputColor = ctx.graph->currentTexture(processedOutputColor);
    }

    if (needsAccumulationPass(*ctx.settings, accumulationPass))
    {
        const int accumulationSampleIndex = ctx.accumulationSampleIndex;
        const float accumulationWeight = (accumulationSampleIndex < ctx.settings->AccumulationTarget)
            ? (1.f / float(std::max(0, accumulationSampleIndex) + 1))
            : 0.0f;
        const ViewInfo* const view = ctx.view;

        denoiseReady = ctx.graph->addPass(
            "Accumulation",
            [outputColor, accumulatedRadiance, processedOutputColor](rg::PassBuilder& setup) {
                setup.read(outputColor, rg::TextureAccess::ShaderResource);
                setup.write(accumulatedRadiance, rg::TextureAccess::UnorderedAccess);
                setup.write(processedOutputColor, rg::TextureAccess::UnorderedAccess);
            },
            [accumulationPass, accumulationWeight, view](rg::RenderPassContext& passCtx) {
                accumulationPass->render(
                    passCtx.commandList(),
                    *view,
                    *view,
                    accumulationWeight);
            });
        outputColor = ctx.graph->currentTexture(outputColor);
        processedOutputColor = ctx.graph->currentTexture(processedOutputColor);
    }

    // Reference OIDN: mid-pass close/execute/wait — keep on primary.
    if (!ctx.settings->RealtimeMode && ctx.settings->ReferenceOIDNDenoiser && ctx.denoise)
    {
        rg::BufferHandle constants{};
        if (ctx.constantBuffer)
            constants = ctx.graph->importBuffer(ctx.constantBuffer, rg::BufferAccess::CopyDest);

        rg::PassOptions oidnOptions{};
        oidnOptions.sideEffect = true;
        oidnOptions.serialOnPrimary = true;
        bool* const commandListWasClosed = ctx.commandListWasClosed;
        FrameConstants* const frameConstants = ctx.frameConstants;
        const caustica::rhi::BufferHandle constantBuffer = ctx.constantBuffer;

        denoiseReady = ctx.graph->addPass(
            "ReferenceOIDN",
            [processedOutputColor, accumulatedRadiance, constants](rg::PassBuilder& setup) {
                setup.read(accumulatedRadiance, rg::TextureAccess::CopySource);
                setup.write(processedOutputColor, rg::TextureAccess::CopyDest);
                if (constants.isValid())
                    setup.write(constants, rg::BufferAccess::CopyDest);
            },
            [denoise, commandListWasClosed, frameConstants, constantBuffer](rg::RenderPassContext& passCtx) {
                const bool closed = denoise->applyReferenceOIDN(passCtx.commandList());
                if (closed && commandListWasClosed)
                    *commandListWasClosed = true;
                // Volatile FrameConstants must be rewritten after reopen for later passes.
                if (closed && frameConstants != nullptr && constantBuffer != nullptr)
                {
                    passCtx.commandList()->writeBuffer(
                        constantBuffer,
                        frameConstants,
                        sizeof(FrameConstants));
                }
            },
            oidnOptions);
    }

    return denoiseReady;
}

} // namespace caustica::render

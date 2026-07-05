#include <render/passes/postProcess/DenoiseAAGraph.h>

#include <render/core/CameraController.h>
#include <render/core/RenderTargets.h>
#include <render/passes/postProcess/AccumulationPass.h>
#include <render/passes/geometry/TemporalAntiAliasingPass.h>
#include <render/worldRenderer/PathTracingFrameContext.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <scene/View.h>

#include <algorithm>
#include <cassert>

namespace caustica::rg
{

namespace
{
    bool needsRealtimeCopyPass(const PathTracerSettings& settings)
    {
        return settings.RealtimeMode && settings.RealtimeAA == 0;
    }

    bool needsTemporalAAPass(const PathTracerSettings& settings, const render::TemporalAntiAliasingPass* taaPass)
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

    render::TemporalAntiAliasingParameters makeTemporalAAParameters(
        const PathTracerSettings& settings,
        const DenoiseAAGraphParams& params)
    {
        render::TemporalAntiAliasingParameters taaParams = settings.TemporalAntiAliasingParams;

        const bool stochasticSplats = settings.EnableGaussianSplats && settings.GaussianSplatSortingMode == 1;
        if (stochasticSplats && params.gaussianSplatTemporalSampleIndex != nullptr)
        {
            taaParams.enableHistoryClamping = false;
            taaParams.useHistoryClampRelax = false;
            taaParams.newFrameWeight = 1.0f / float(*params.gaussianSplatTemporalSampleIndex + 1);
        }

        return taaParams;
    }

    bool computeTemporalFeedbackValid(const DenoiseAAGraphParams& params)
    {
        const bool stochasticSplats = params.settings->EnableGaussianSplats && params.settings->GaussianSplatSortingMode == 1;
        const bool stochasticReset = stochasticSplats && params.gaussianSplatTemporalReset != nullptr
            && (params.aaReset || params.settings->ResetAccumulation || params.settings->ResetRealtimeCaches
                || *params.gaussianSplatTemporalReset);
        return !params.aaReset && !stochasticReset && params.frameIndex != 0;
    }

    void prepareDenoiseAAGraphState(const DenoiseAAGraphParams& params)
    {
        if (!params.settings || !params.settings->RealtimeMode || params.settings->RealtimeAA != 1)
            return;

        const bool stochasticSplats = params.settings->EnableGaussianSplats && params.settings->GaussianSplatSortingMode == 1;
        if (!stochasticSplats || params.gaussianSplatTemporalReset == nullptr || params.gaussianSplatTemporalSampleIndex == nullptr)
            return;

        const bool stochasticReset = params.aaReset || params.settings->ResetAccumulation || params.settings->ResetRealtimeCaches
            || *params.gaussianSplatTemporalReset;
        if (stochasticReset)
            *params.gaussianSplatTemporalSampleIndex = 0;
    }
}

void buildDenoiseAndAAGraph(const DenoiseAAGraphParams& params)
{
    assert(params.renderTargets);
    assert(params.settings);

    prepareDenoiseAAGraphState(params);

    GraphBuilder& graph = params.graph;
    RenderTargets* targets = params.renderTargets;

    const TextureHandle outputColor = graph.importTexture(
        targets->outputColor,
        TextureAccess::UnorderedAccess);
    const TextureHandle processedOutputColor = graph.importTexture(
        targets->processedOutputColor,
        TextureAccess::UnorderedAccess);
    const TextureHandle accumulatedRadiance = graph.importTexture(
        targets->accumulatedRadiance,
        TextureAccess::UnorderedAccess);
    const TextureHandle motionVectors = graph.importTexture(
        targets->screenMotionVectors,
        TextureAccess::ShaderResource);
    const TextureHandle temporalFeedback1 = graph.importTexture(
        targets->temporalFeedback1,
        TextureAccess::UnorderedAccess);
    const TextureHandle temporalFeedback2 = graph.importTexture(
        targets->temporalFeedback2,
        TextureAccess::UnorderedAccess);
    const TextureHandle historyClampRelax = graph.importTexture(
        targets->combinedHistoryClampRelax,
        TextureAccess::ShaderResource);

    graph.extractTexture(outputColor, TextureAccess::UnorderedAccess);
    graph.extractTexture(processedOutputColor, TextureAccess::UnorderedAccess);
    graph.extractTexture(accumulatedRadiance, TextureAccess::UnorderedAccess);
    graph.extractTexture(temporalFeedback1, TextureAccess::UnorderedAccess);
    graph.extractTexture(temporalFeedback2, TextureAccess::UnorderedAccess);

    if (needsNoDenoiserFinalMergePass(*params.settings))
    {
        assert(params.worldRenderer);

        const TextureHandle stableRadiance = graph.importTexture(
            targets->stableRadiance,
            TextureAccess::ShaderResource);
        const TextureHandle stablePlanesHeader = graph.importTexture(
            targets->stablePlanesHeader,
            TextureAccess::ShaderResource);
        const BufferHandle stablePlanesBuffer = graph.importBuffer(
            targets->stablePlanesBuffer,
            BufferAccess::ShaderResource);

        graph.addPass(
            "NoDenoiserFinalMerge",
            [&](PassBuilder& setup) {
                setup.read(outputColor, TextureAccess::UnorderedAccess);
                setup.read(stableRadiance, TextureAccess::ShaderResource);
                setup.read(stablePlanesHeader, TextureAccess::ShaderResource);
                setup.read(stablePlanesBuffer, BufferAccess::ShaderResource);
                setup.write(outputColor, TextureAccess::UnorderedAccess);
            },
            [params](RenderPassContext& ctx) {
                params.worldRenderer->runNoDenoiserFinalMerge(ctx.commandList());
            },
            PassOptions{ .sideEffect = true });
    }

    if (needsRealtimeCopyPass(*params.settings))
    {
        graph.addPass(
            "CopyOutputToProcessed",
            [&](PassBuilder& setup) {
                setup.read(outputColor, TextureAccess::CopySource);
                setup.write(processedOutputColor, TextureAccess::CopyDest);
            },
            [outputColor, processedOutputColor](RenderPassContext& ctx) {
                ctx.commandList()->copyTexture(
                    ctx.texture(processedOutputColor),
                    nvrhi::TextureSlice(),
                    ctx.texture(outputColor),
                    nvrhi::TextureSlice());
            });
    }

    if (needsStochasticGaussianSplatsBeforeAA(*params.settings))
    {
        assert(params.worldRenderer);

        const TextureHandle depth = graph.importTexture(
            targets->depth,
            TextureAccess::ShaderResource);

        graph.addPass(
            "Gaussian Splats Stochastic",
            [&](PassBuilder& setup) {
                setup.read(outputColor, TextureAccess::UnorderedAccess);
                setup.read(depth, TextureAccess::ShaderResource);
                setup.write(outputColor, TextureAccess::UnorderedAccess);
            },
            [params](RenderPassContext& ctx) {
                params.worldRenderer->renderGaussianSplats(ctx.commandList(), true);
            },
            PassOptions{ .sideEffect = true });
    }

    if (needsTemporalAAPass(*params.settings, params.temporalAAPass))
    {
        assert(params.camera);

        const auto taaParams = makeTemporalAAParameters(*params.settings, params);
        const bool feedbackIsValid = computeTemporalFeedbackValid(params);
        const bool stochasticSplats = params.settings->EnableGaussianSplats && params.settings->GaussianSplatSortingMode == 1;

        graph.addPass(
            "TAA",
            [&](PassBuilder& setup) {
                setup.read(outputColor, TextureAccess::ShaderResource);
                setup.read(motionVectors, TextureAccess::ShaderResource);
                setup.read(temporalFeedback1, TextureAccess::ShaderResource);
                setup.read(temporalFeedback2, TextureAccess::UnorderedAccess);
                setup.read(historyClampRelax, TextureAccess::ShaderResource);
                setup.write(processedOutputColor, TextureAccess::UnorderedAccess);
                setup.write(temporalFeedback1, TextureAccess::UnorderedAccess);
                setup.write(temporalFeedback2, TextureAccess::UnorderedAccess);
            },
            [params, taaParams, feedbackIsValid, stochasticSplats](RenderPassContext& ctx) {
                // Must use the live path-trace view (render viewport + current jitter), not the
                // Extract-phase postProcessView snapshot which uses display viewport.
                const ICompositeView& taaView = *params.camera->view();
                params.temporalAAPass->TemporalResolve(
                    ctx.commandList(),
                    taaParams,
                    feedbackIsValid,
                    taaView,
                    taaView);

                if (stochasticSplats && params.gaussianSplatTemporalSampleIndex != nullptr
                    && params.gaussianSplatTemporalReset != nullptr)
                {
                    *params.gaussianSplatTemporalSampleIndex =
                        std::min(*params.gaussianSplatTemporalSampleIndex + 1, 1024 * 1024);
                    *params.gaussianSplatTemporalReset = false;
                }
            });
    }

    if (needsDlssPass(*params.settings))
    {
        assert(params.worldRenderer);

        const bool dlssRayReconstruction = params.settings->RealtimeAA == 3;
        const TextureHandle depth = graph.importTexture(
            targets->depth,
            TextureAccess::ShaderResource);
        const TextureHandle preUIColor = graph.importTexture(
            targets->preUIColor,
            TextureAccess::ShaderResource);

        graph.addPass(
            dlssRayReconstruction ? "DLSS-RR" : "DLSS",
            [&](PassBuilder& setup) {
                setup.read(outputColor, TextureAccess::ShaderResource);
                setup.read(motionVectors, TextureAccess::ShaderResource);
                setup.read(depth, TextureAccess::ShaderResource);
                setup.read(preUIColor, TextureAccess::ShaderResource);
                setup.write(processedOutputColor, TextureAccess::UnorderedAccess);

                if (dlssRayReconstruction)
                {
                    const TextureHandle rrDiffuseAlbedo = graph.importTexture(
                        targets->rrDiffuseAlbedo,
                        TextureAccess::ShaderResource);
                    const TextureHandle rrSpecAlbedo = graph.importTexture(
                        targets->rrSpecAlbedo,
                        TextureAccess::ShaderResource);
                    const TextureHandle rrNormalsAndRoughness = graph.importTexture(
                        targets->rrNormalsAndRoughness,
                        TextureAccess::ShaderResource);
                    setup.read(rrDiffuseAlbedo, TextureAccess::ShaderResource);
                    setup.read(rrSpecAlbedo, TextureAccess::ShaderResource);
                    setup.read(rrNormalsAndRoughness, TextureAccess::ShaderResource);
                }
            },
            [params](RenderPassContext& ctx) {
                params.worldRenderer->runDlssUpscale(ctx.commandList(), params.aaReset);
            },
            PassOptions{ .sideEffect = true });
    }

    if (needsAccumulationPass(*params.settings, params.accumulationPass))
    {
        assert(params.camera);

        const float accumulationWeight = (params.accumulationSampleIndex < params.settings->AccumulationTarget)
            ? (1.f / float(std::max(0, params.accumulationSampleIndex) + 1))
            : 0.0f;

        graph.addPass(
            "Accumulation",
            [&](PassBuilder& setup) {
                setup.read(outputColor, TextureAccess::ShaderResource);
                setup.write(accumulatedRadiance, TextureAccess::UnorderedAccess);
                setup.write(processedOutputColor, TextureAccess::UnorderedAccess);
            },
            [params, accumulationWeight](RenderPassContext& ctx) {
                const IView& view = *params.camera->view();
                params.accumulationPass->Render(
                    ctx.commandList(),
                    view,
                    view,
                    accumulationWeight);
            });
    }

    if (needsGaussianSplatsCompositePass(*params.settings))
    {
        assert(params.worldRenderer);

        const TextureHandle depth = graph.importTexture(
            targets->depth,
            TextureAccess::ShaderResource);

        graph.addPass(
            "Gaussian Splats",
            [&](PassBuilder& setup) {
                setup.read(processedOutputColor, TextureAccess::UnorderedAccess);
                setup.read(depth, TextureAccess::ShaderResource);
                setup.write(processedOutputColor, TextureAccess::UnorderedAccess);
            },
            [params](RenderPassContext& ctx) {
                params.worldRenderer->renderGaussianSplats(ctx.commandList(), false);
            },
            PassOptions{ .sideEffect = true });
    }

    if (params.framePassRegistry && params.frameContext)
    {
        params.framePassRegistry->applyGraphPasses(
            render::FramePassInsertPoint::AfterDenoiseAndAA,
            graph,
            *params.frameContext);
    }
}

} // namespace caustica::rg

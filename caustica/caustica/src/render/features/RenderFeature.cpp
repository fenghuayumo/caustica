#include <render/features/RenderFeature.h>

#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/features/RenderFeatureContext.h>

namespace caustica::render
{

namespace
{
    struct GraphFeatureRegistration
    {
        const char* name = nullptr;
        void (*registerFn)(RenderFeatureContext) = nullptr;
    };

    void registerClearFrameTargetsPass(RenderFeatureContext ctx)
    {
        if (!ctx.graph || !ctx.renderTargets)
            return;

        const rg::TextureHandle depth = ctx.graph->importTexture(
            ctx.renderTargets->depth,
            rg::TextureAccess::UnorderedAccess);
        const rg::TextureHandle combinedHistoryClampRelax = ctx.graph->importTexture(
            ctx.renderTargets->combinedHistoryClampRelax,
            rg::TextureAccess::UnorderedAccess);

        ctx.graph->addPass(
            "ClearFrameTargets",
            [depth, combinedHistoryClampRelax](rg::PassBuilder& setup) {
                setup.write(depth, rg::TextureAccess::UnorderedAccess);
                setup.write(combinedHistoryClampRelax, rg::TextureAccess::UnorderedAccess);
            },
            [ctx](rg::RenderPassContext& passCtx) {
                ctx.renderTargets->clear(passCtx.commandList());
            },
            rg::PassOptions{ .sideEffect = true });

        if (!ctx.hasScene)
        {
            const rg::TextureHandle outputColor = ctx.graph->importTexture(
                ctx.renderTargets->outputColor,
                rg::TextureAccess::UnorderedAccess);

            ctx.graph->addPass(
                "ClearNoSceneOutput",
                [outputColor](rg::PassBuilder& setup) {
                    setup.write(outputColor, rg::TextureAccess::UnorderedAccess);
                },
                [outputColor](rg::RenderPassContext& passCtx) {
                    passCtx.commandList()->clearTextureFloat(
                        passCtx.texture(outputColor),
                        nvrhi::AllSubresources,
                        nvrhi::Color(1, 1, 0, 0));
                },
                rg::PassOptions{ .sideEffect = true, .executeAfter = "ClearFrameTargets" });
        }
    }

    constexpr GraphFeatureRegistration kDefaultGraphFeatures[] = {
        { "FrameClear", registerClearFrameTargetsPass },
        { "RtxdiBeginFrame", registerRtxdiBeginFrameFeature },
        { "PathTracePrePass", registerPathTracePrePassFeature },
        { "VBufferExport", registerVBufferExportFeature },
        { "PathTraceLightingEnd", registerPathTraceLightingEndFeature },
        { "GaussianSplatAccelBuild", registerGaussianSplatAccelBuildFeature },
        { "MainPathTrace", registerMainPathTraceFeature },
        { "Rtxdi", registerRtxdiExecuteFeature },
        { "DenoiserPrepare", registerDenoiserPrepareFeature },
        { "NRD", registerNrdFeature },
        { "GaussianSplatPreAA", registerGaussianSplatPreAAFeature },
        { "DenoiseAA", registerDenoiseAAFeature },
        { "GaussianSplatComposite", registerGaussianSplatCompositeFeature },
        { "PostProcess", registerPostProcessFeature },
        { "Composite", registerCompositeFeature },
        { "DebugOverlay", registerDebugOverlayFeature },
    };
}

void registerDefaultGraphFeatures(RenderFeatureContext ctx)
{
    for (const GraphFeatureRegistration& feature : kDefaultGraphFeatures)
    {
        if (feature.registerFn)
            feature.registerFn(ctx);
    }
}

} // namespace caustica::render

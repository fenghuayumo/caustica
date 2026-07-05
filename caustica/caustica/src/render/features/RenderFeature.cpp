#include <render/features/RenderFeature.h>

#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/features/RenderFeatureContext.h>
#include <render/features/CompositeFeature.h>
#include <render/features/DenoiseAAFeature.h>
#include <render/features/NrdFeature.h>
#include <render/features/PathTraceFeature.h>
#include <render/features/PostProcessFeature.h>

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
    }

    constexpr GraphFeatureRegistration kDefaultGraphFeatures[] = {
        { "FrameClear", registerClearFrameTargetsPass },
        { "PathTrace", registerPathTraceFeature },
        { "NRD", registerNrdFeature },
        { "DenoiseAA", registerDenoiseAAFeature },
        { "PostProcess", registerPostProcessFeature },
        { "Composite", registerCompositeFeature },
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

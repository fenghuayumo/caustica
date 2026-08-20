#include <render/FrameGraphPasses.h>

#include <render/FrameGraphContext.h>
#include <render/core/FullscreenBlitPass.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/geometry/BloomPass.h>
#include <render/passes/lighting/distant/EnvMapProcessor.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <shaders/FrameConstantBuffer.h>

#include <cassert>

namespace caustica::render
{

namespace
{
    void registerAerialPerspectivePass(
        rg::TextureHandle processedOutputColor,
        FrameGraphContext ctx)
    {
        auto environment = ctx.environment;
        if (!environment || !environment->isProcedural() || !ctx.settings->EnvironmentMapParams.enabled)
            return;

        const std::shared_ptr<SampleProceduralSky>& sky = environment->getProceduralSky();
        if (!sky || !sky->isAerialPerspectiveEnabled())
            return;

        const rg::TextureHandle depth = ctx.graph->importTexture(
            ctx.renderTargets->depth,
            rg::TextureAccess::ShaderResource);
        const dm::uint2 displaySize = ctx.extractedView->displaySize;
        const caustica::PlanarView postProcessView = ctx.extractedView->postProcessView;
        const auto tintColor = ctx.settings->EnvironmentMapParams.TintColor;
        const float intensity = ctx.settings->EnvironmentMapParams.Intensity;
        const auto rotation = ctx.settings->EnvironmentMapParams.RotationXYZ;

        ctx.graph->addPass(
            "SkyAerialPerspective",
            [processedOutputColor, depth](rg::PassBuilder& setup) {
                setup.read(processedOutputColor, rg::TextureAccess::UnorderedAccess);
                setup.read(depth, rg::TextureAccess::ShaderResource);
                setup.write(processedOutputColor, rg::TextureAccess::UnorderedAccess);
            },
            [processedOutputColor, depth, sky, displaySize, postProcessView,
             tintColor, intensity, rotation](rg::RenderPassContext& passCtx) {
                sky->applyAerialPerspective(
                    passCtx.commandList(),
                    passCtx.texture(processedOutputColor),
                    passCtx.texture(depth),
                    postProcessView,
                    displaySize.x,
                    displaySize.y,
                    tintColor,
                    intensity,
                    rotation);
            });
    }

    bool isBloomEnabled(const PathTracerSettings& settings)
    {
        return settings.EnableBloom
            && settings.BloomIntensity > 0.f
            && settings.BloomRadius > 0.f;
    }

    void registerEdgeDetectionGraphPasses(
        rg::TextureHandle ldrColor,
        rg::TextureHandle ldrColorScratch,
        FrameGraphContext ctx,
        bool enabled)
    {
        assert(ctx.graph);
        PTPipelineVariant* const pipeline = ctx.ptEdgeDetection;
        const dm::uint2 displaySize = ctx.extractedView->displaySize;
        const caustica::rhi::BindingSetHandle bindingSet = ctx.bindingSet;
        caustica::rhi::DescriptorTable* const descriptorTable = ctx.descriptorTable;
        const float threshold = ctx.settings->PostProcessEdgeDetectionThreshold;

        ctx.graph->addPass(
            "PPEdgeDetectionCopy",
            [ldrColor, ldrColorScratch](rg::PassBuilder& setup) {
                setup.read(ldrColor, rg::TextureAccess::CopySource);
                setup.write(ldrColorScratch, rg::TextureAccess::CopyDest);
            },
            [ldrColor, ldrColorScratch](rg::RenderPassContext& passCtx) {
                passCtx.commandList()->copyTexture(
                    passCtx.texture(ldrColorScratch),
                    caustica::rhi::TextureSlice(),
                    passCtx.texture(ldrColor),
                    caustica::rhi::TextureSlice());
            },
            rg::PassOptions{ .enabled = enabled });

        ctx.graph->addPass(
            "PPEdgeDetection",
            [ldrColor](rg::PassBuilder& setup) {
                setup.write(ldrColor, rg::TextureAccess::UnorderedAccess);
            },
            [pipeline, displaySize, bindingSet, descriptorTable,
             threshold](rg::RenderPassContext& passCtx) {
                assert(pipeline);
                if (!pipeline->hasPipeline() || !pipeline->getShaderTable())
                    return;

                caustica::rhi::rt::DispatchRaysArguments args;
                args.width = displaySize.x;
                args.height = displaySize.y;

                caustica::rhi::rt::State state;
                state.shaderTable = pipeline->getShaderTable();
                state.bindings = { bindingSet, descriptorTable };
                passCtx.commandList()->setRayTracingState(state);

                FrameMiniConstants miniConstants = {
                    uint4(*reinterpret_cast<const uint*>(&threshold), 0, 0, 0)
                };
                passCtx.commandList()->setPushConstants(&miniConstants, sizeof(miniConstants));
                passCtx.commandList()->dispatchRays(args);
            },
            rg::PassOptions{ .enabled = enabled });
    }
}

void registerPostProcess(FrameGraphContext ctx)
{
    assert(ctx.extractedView);
    assert(ctx.renderTargets);
    assert(ctx.settings);
    assert(ctx.graph);

    ToneMappingPass* toneMappingPass = ctx.toneMapping;
    assert(toneMappingPass);

    RenderTargets& targets = *ctx.renderTargets;

    const rg::TextureHandle processedOutputColor = ctx.graph->importTexture(
        targets.processedOutputColor,
        caustica::rhi::ResourceStates::UnorderedAccess);
    const rg::TextureHandle ldrColor = ctx.graph->importTexture(
        targets.ldrColor,
        caustica::rhi::ResourceStates::ShaderResource);
    const rg::TextureHandle ldrColorScratch = ctx.graph->importTexture(
        targets.ldrColorScratch,
        caustica::rhi::ResourceStates::Common);
    ctx.graph->extractTexture(processedOutputColor, rg::TextureAccess::UnorderedAccess);
    ctx.graph->extractTexture(ldrColor, rg::TextureAccess::ShaderResource);
    ctx.graph->extractTexture(ldrColorScratch, rg::TextureAccess::ShaderResource);

    registerAerialPerspectivePass(processedOutputColor, ctx);

    BloomPass* bloomPass = ctx.bloom;
    if (bloomPass != nullptr)
    {
        bloomPass->registerGraphPass(
            *ctx.graph,
            processedOutputColor,
            targets.processedOutputFramebuffer,
            ctx.extractedView->postProcessView,
            ctx.settings->BloomRadius,
            ctx.settings->BloomIntensity,
            isBloomEnabled(*ctx.settings));
    }

    toneMappingPass->registerGraphPass(
        *ctx.graph,
        processedOutputColor,
        ldrColor,
        ctx.extractedView->postProcessView,
        ctx.settings->EnableToneMapping,
        ctx.commandListWasClosed);

    // Preserve the photographed/display-referred Gaussian appearance while
    // keeping mesh lighting in the normal HDR tone-mapping path.
    if (!ctx.settings->GaussianSplatApplyToneMapping)
        (void)registerGaussianSplatCompositePass(ctx);

    registerEdgeDetectionGraphPasses(
        ldrColor,
        ldrColorScratch,
        ctx,
        ctx.settings->PostProcessEdgeDetection && ctx.ptEdgeDetection != nullptr);
}

rg::PassHandle registerCompositeGraphPasses(FrameGraphContext ctx)
{
    assert(ctx.targetFramebuffer);
    assert(ctx.bindingCache);
    assert(ctx.blitPass);
    assert(ctx.renderTargets);
    assert(ctx.graph);

    const rg::TextureHandle ldrColor = ctx.graph->importTexture(
        ctx.renderTargets->ldrColor,
        caustica::rhi::ResourceStates::ShaderResource);

    caustica::rhi::Texture* targetColor = ctx.targetFramebuffer->getDesc().colorAttachments[0].texture;
    assert(targetColor);

    const rg::TextureHandle targetColorHandle = ctx.graph->importTexture(targetColor, rg::TextureAccess::RenderTarget);
    ctx.graph->extractTexture(targetColorHandle, rg::TextureAccess::RenderTarget);
    FullscreenBlitPass* const blitPass = ctx.blitPass;
    caustica::rhi::Framebuffer* const targetFramebuffer = ctx.targetFramebuffer;

    return ctx.graph->addPass(
        "Blit",
        [ldrColor, targetColorHandle](rg::PassBuilder& setup) {
            setup.read(ldrColor, rg::TextureAccess::ShaderResource);
            setup.write(targetColorHandle, rg::TextureAccess::RenderTarget);
        },
        [blitPass, targetFramebuffer, ldrColor](rg::RenderPassContext& passCtx) {
            BlitParameters blitParams{};
            blitParams.targetFramebuffer = targetFramebuffer;
            blitParams.sourceTexture = passCtx.texture(ldrColor);
            blitPass->blitTexture(passCtx.commandList(), blitParams, nullptr);
        },
        rg::PassOptions{ .sideEffect = true });
}

void registerPostProcessGraphPasses(FrameGraphContext ctx)
{
    registerPostProcess(ctx);
}

} // namespace caustica::render

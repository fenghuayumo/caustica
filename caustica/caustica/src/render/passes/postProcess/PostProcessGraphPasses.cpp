#include <render/FrameGraphPasses.h>

#include <render/FrameGraphContext.h>
#include <render/core/FullscreenBlitPass.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/geometry/BloomPass.h>
#include <render/passes/lighting/distant/EnvMapProcessor.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <shaders/SampleConstantBuffer.h>

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

        ctx.graph->addPass(
            "SkyAerialPerspective",
            [processedOutputColor, depth](rg::PassBuilder& setup) {
                setup.read(processedOutputColor, rg::TextureAccess::UnorderedAccess);
                setup.read(depth, rg::TextureAccess::ShaderResource);
                setup.write(processedOutputColor, rg::TextureAccess::UnorderedAccess);
            },
            [processedOutputColor, depth, sky, ctx](rg::RenderPassContext& passCtx) {
                const dm::uint2 size = ctx.extractedView->displaySize;
                sky->applyAerialPerspective(
                    passCtx.commandList(),
                    passCtx.texture(processedOutputColor),
                    passCtx.texture(depth),
                    ctx.extractedView->postProcessView,
                    size.x,
                    size.y,
                    ctx.settings->EnvironmentMapParams.TintColor,
                    ctx.settings->EnvironmentMapParams.Intensity,
                    ctx.settings->EnvironmentMapParams.RotationXYZ);
            });
    }

    bool isBloomEnabled(const PathTracerSettings& settings)
    {
        return settings.EnableBloom
            && settings.BloomIntensity > 0.f
            && settings.BloomRadius > 0.f;
    }

    void registerTestRaygenHdrPass(
        rg::TextureHandle processedOutputColor,
        FrameGraphContext ctx,
        bool enabled)
    {
        assert(ctx.graph);

        ctx.graph->addPass(
            "TestRaygenPP_HDR",
            [processedOutputColor](rg::PassBuilder& setup) {
                setup.write(processedOutputColor, rg::TextureAccess::UnorderedAccess);
            },
            [processedOutputColor, ctx](rg::RenderPassContext& passCtx) {
                assert(ctx.extractedView);
                PTPipelineVariant* pipeline = ctx.ptTestRaygenPPHDR;
                assert(pipeline);

                nvrhi::rt::DispatchRaysArguments args;
                args.width = ctx.extractedView->displaySize.x;
                args.height = ctx.extractedView->displaySize.y;

                nvrhi::rt::State state;
                state.shaderTable = pipeline->getShaderTable();
                state.bindings = { ctx.bindingSet, ctx.descriptorTable };
                passCtx.commandList()->setRayTracingState(state);

                SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
                passCtx.commandList()->setPushConstants(&miniConstants, sizeof(miniConstants));
                passCtx.commandList()->dispatchRays(args);
                (void)passCtx.texture(processedOutputColor);
            },
            rg::PassOptions{ .enabled = enabled });
    }

    void registerEdgeDetectionGraphPasses(
        rg::TextureHandle ldrColor,
        rg::TextureHandle ldrColorScratch,
        FrameGraphContext ctx,
        bool enabled)
    {
        assert(ctx.graph);

        ctx.graph->addPass(
            "PPEdgeDetectionCopy",
            [ldrColor, ldrColorScratch](rg::PassBuilder& setup) {
                setup.read(ldrColor, rg::TextureAccess::CopySource);
                setup.write(ldrColorScratch, rg::TextureAccess::CopyDest);
            },
            [ldrColor, ldrColorScratch](rg::RenderPassContext& passCtx) {
                passCtx.commandList()->copyTexture(
                    passCtx.texture(ldrColorScratch),
                    nvrhi::TextureSlice(),
                    passCtx.texture(ldrColor),
                    nvrhi::TextureSlice());
            },
            rg::PassOptions{ .enabled = enabled });

        ctx.graph->addPass(
            "PPEdgeDetection",
            [ldrColor](rg::PassBuilder& setup) {
                setup.write(ldrColor, rg::TextureAccess::UnorderedAccess);
            },
            [ctx](rg::RenderPassContext& passCtx) {
                assert(ctx.extractedView);
                assert(ctx.settings);
                PTPipelineVariant* pipeline = ctx.ptEdgeDetection;
                assert(pipeline);

                nvrhi::rt::DispatchRaysArguments args;
                args.width = ctx.extractedView->displaySize.x;
                args.height = ctx.extractedView->displaySize.y;

                nvrhi::rt::State state;
                state.shaderTable = pipeline->getShaderTable();
                state.bindings = { ctx.bindingSet, ctx.descriptorTable };
                passCtx.commandList()->setRayTracingState(state);

                SampleMiniConstants miniConstants = {
                    uint4(*reinterpret_cast<uint*>(&ctx.settings->PostProcessEdgeDetectionThreshold), 0, 0, 0)
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
        nvrhi::ResourceStates::UnorderedAccess);
    const rg::TextureHandle ldrColor = ctx.graph->importTexture(
        targets.ldrColor,
        nvrhi::ResourceStates::ShaderResource);
    const rg::TextureHandle ldrColorScratch = ctx.graph->importTexture(
        targets.ldrColorScratch,
        nvrhi::ResourceStates::Common);
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

    registerTestRaygenHdrPass(
        processedOutputColor,
        ctx,
        ctx.settings->PostProcessTestPassHDR && ctx.ptTestRaygenPPHDR != nullptr);

    toneMappingPass->registerGraphPass(
        *ctx.graph,
        processedOutputColor,
        ldrColor,
        ctx.extractedView->postProcessView,
        ctx.settings->EnableToneMapping,
        ctx.commandListWasClosed);

    registerEdgeDetectionGraphPasses(
        ldrColor,
        ldrColorScratch,
        ctx,
        ctx.settings->PostProcessEdgeDetection && ctx.ptEdgeDetection != nullptr);
}

void registerCompositeGraphPasses(FrameGraphContext ctx)
{
    assert(ctx.targetFramebuffer);
    assert(ctx.bindingCache);
    assert(ctx.blitPass);
    assert(ctx.renderTargets);
    assert(ctx.graph);

    const rg::TextureHandle ldrColor = ctx.graph->importTexture(
        ctx.renderTargets->ldrColor,
        nvrhi::ResourceStates::ShaderResource);

    nvrhi::ITexture* targetColor = ctx.targetFramebuffer->getDesc().colorAttachments[0].texture;
    assert(targetColor);

    const rg::TextureHandle targetColorHandle = ctx.graph->importTexture(targetColor, rg::TextureAccess::RenderTarget);
    ctx.graph->extractTexture(targetColorHandle, rg::TextureAccess::RenderTarget);

    ctx.graph->addPass(
        "Blit",
        [ldrColor, targetColorHandle](rg::PassBuilder& setup) {
            setup.read(ldrColor, rg::TextureAccess::ShaderResource);
            setup.write(targetColorHandle, rg::TextureAccess::RenderTarget);
        },
        [ctx, ldrColor](rg::RenderPassContext& passCtx) {
            BlitParameters blitParams{};
            blitParams.targetFramebuffer = ctx.targetFramebuffer;
            blitParams.sourceTexture = passCtx.texture(ldrColor);
            ctx.blitPass->blitTexture(passCtx.commandList(), blitParams, nullptr);
        },
        rg::PassOptions{ .sideEffect = true });
}

void registerPostProcessGraphPasses(FrameGraphContext ctx)
{
    registerPostProcess(ctx);
}

} // namespace caustica::render

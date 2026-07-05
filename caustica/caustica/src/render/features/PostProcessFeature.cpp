#include <render/features/RenderFeature.h>

#include <render/core/PathTracingShaderCompiler.h>
#include <render/core/RenderTargets.h>
#include <render/features/RenderFeatureContext.h>
#include <render/graph/GraphBuilder.h>
#include <render/features/RenderFeatureContext.h>
#include <render/passes/geometry/BloomPass.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <shaders/SampleConstantBuffer.h>

#include <cassert>

namespace caustica::render
{

namespace
{
    bool isBloomEnabled(const PathTracerSettings& settings)
    {
        return settings.EnableBloom
            && settings.BloomIntensity > 0.f
            && settings.BloomRadius > 0.f;
    }

    void registerTestRaygenHdrPass(
        rg::TextureHandle processedOutputColor,
        RenderFeatureContext ctx,
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
                assert(ctx.renderer);
                PTPipelineVariant* pipeline = ctx.renderer->ptPipelineTestRaygenPPHDR().get();
                assert(pipeline);

                nvrhi::rt::DispatchRaysArguments args;
                args.width = ctx.extractedView->displaySize.x;
                args.height = ctx.extractedView->displaySize.y;

                nvrhi::rt::State state;
                state.shaderTable = pipeline->getShaderTable();
                state.bindings = {
                    ctx.renderer->getBindingSet(),
                    ctx.renderer->getPathTracingContext().descriptorTable
                        ? ctx.renderer->getPathTracingContext().descriptorTable->getDescriptorTable()
                        : nullptr
                };
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
        RenderFeatureContext ctx,
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
                assert(ctx.renderer);
                assert(ctx.settings);
                PTPipelineVariant* pipeline = ctx.renderer->ptPipelineEdgeDetection().get();
                assert(pipeline);

                nvrhi::rt::DispatchRaysArguments args;
                args.width = ctx.extractedView->displaySize.x;
                args.height = ctx.extractedView->displaySize.y;

                nvrhi::rt::State state;
                state.shaderTable = pipeline->getShaderTable();
                state.bindings = {
                    ctx.renderer->getBindingSet(),
                    ctx.renderer->getPathTracingContext().descriptorTable
                        ? ctx.renderer->getPathTracingContext().descriptorTable->getDescriptorTable()
                        : nullptr
                };
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

void registerPostProcessFeature(RenderFeatureContext ctx)
{
    assert(ctx.extractedView);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);
    assert(ctx.graph);

    ToneMappingPass* toneMappingPass = ctx.renderer->getToneMappingPass();
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

    BloomPass* bloomPass = ctx.renderer->getBloomPass();
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
        ctx.settings->PostProcessTestPassHDR && ctx.renderer->ptPipelineTestRaygenPPHDR() != nullptr);

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
        ctx.settings->PostProcessEdgeDetection && ctx.renderer->ptPipelineEdgeDetection() != nullptr);
}

} // namespace caustica::render

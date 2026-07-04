#include <render/graph/PostProcessGraph.h>

#include <render/Core/CameraController.h>
#include <render/Core/PathTracingShaderCompiler.h>
#include <render/Core/RenderTargets.h>
#include <render/Passes/Geometry/BloomPass.h>
#include <render/Passes/PostProcess/ToneMappingPasses.h>
#include <scene/View.h>
#include <shaders/SampleConstantBuffer.h>

#include <cassert>

namespace caustica::rg
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
        GraphBuilder& graph,
        TextureHandle processedOutputColor,
        const PostProcessGraphParams& params,
        bool enabled)
    {
        graph.addPass(
            "TestRaygenPP_HDR",
            [&](PassBuilder& setup) {
                setup.write(processedOutputColor, TextureAccess::UnorderedAccess);
            },
            [processedOutputColor, params](RenderPassContext& ctx) {
                assert(params.testRaygenPpHdrPipeline);
                assert(params.renderTargets);

                nvrhi::rt::DispatchRaysArguments args;
                args.width = params.displaySize.x;
                args.height = params.displaySize.y;

                nvrhi::rt::State state;
                state.shaderTable = params.testRaygenPpHdrPipeline->GetShaderTable();
                state.bindings = { params.pathTracingBindingSet, params.descriptorTable };
                ctx.commandList()->setRayTracingState(state);

                SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
                ctx.commandList()->setPushConstants(&miniConstants, sizeof(miniConstants));
                ctx.commandList()->dispatchRays(args);
                (void)ctx.texture(processedOutputColor);
            },
            enabled);
    }

    void registerEdgeDetectionPasses(
        GraphBuilder& graph,
        TextureHandle ldrColor,
        TextureHandle ldrColorScratch,
        const PostProcessGraphParams& params,
        bool enabled)
    {
        graph.addPass(
            "PPEdgeDetectionCopy",
            [&](PassBuilder& setup) {
                setup.read(ldrColor, TextureAccess::CopySource);
                setup.write(ldrColorScratch, TextureAccess::CopyDest);
            },
            [ldrColor, ldrColorScratch](RenderPassContext& ctx) {
                ctx.commandList()->copyTexture(
                    ctx.texture(ldrColorScratch),
                    nvrhi::TextureSlice(),
                    ctx.texture(ldrColor),
                    nvrhi::TextureSlice());
            },
            enabled);

        graph.addPass(
            "PPEdgeDetection",
            [&](PassBuilder& setup) {
                setup.write(ldrColor, TextureAccess::UnorderedAccess);
            },
            [params](RenderPassContext& ctx) {
                assert(params.edgeDetectionPipeline);

                nvrhi::rt::DispatchRaysArguments args;
                args.width = params.displaySize.x;
                args.height = params.displaySize.y;

                nvrhi::rt::State state;
                state.shaderTable = params.edgeDetectionPipeline->GetShaderTable();
                state.bindings = { params.pathTracingBindingSet, params.descriptorTable };
                ctx.commandList()->setRayTracingState(state);

                SampleMiniConstants miniConstants = {
                    uint4(*reinterpret_cast<uint*>(&params.settings->PostProcessEdgeDetectionThreshold), 0, 0, 0)
                };
                ctx.commandList()->setPushConstants(&miniConstants, sizeof(miniConstants));
                ctx.commandList()->dispatchRays(args);
            },
            enabled);
    }
}

void buildPostProcessGraph(const PostProcessGraphParams& params)
{
    assert(params.renderTargets);
    assert(params.settings);
    assert(params.compositeView);
    assert(params.toneMappingPass);

    GraphBuilder& graph = params.graph;

    const TextureHandle processedOutputColor = graph.importTexture(
        params.renderTargets->ProcessedOutputColor,
        nvrhi::ResourceStates::UnorderedAccess);
    const TextureHandle ldrColor = graph.importTexture(
        params.renderTargets->LdrColor,
        nvrhi::ResourceStates::ShaderResource);
    const TextureHandle ldrColorScratch = graph.importTexture(
        params.renderTargets->LdrColorScratch,
        nvrhi::ResourceStates::Common);

    if (params.bloomPass != nullptr)
    {
        params.bloomPass->registerGraphPass(
            graph,
            processedOutputColor,
            params.renderTargets->ProcessedOutputFramebuffer,
            *params.compositeView,
            params.settings->BloomRadius,
            params.settings->BloomIntensity,
            isBloomEnabled(*params.settings));
    }

    registerTestRaygenHdrPass(
        graph,
        processedOutputColor,
        params,
        params.settings->PostProcessTestPassHDR && params.testRaygenPpHdrPipeline != nullptr);

    params.toneMappingPass->registerGraphPass(
        graph,
        processedOutputColor,
        ldrColor,
        *params.compositeView,
        params.settings->EnableToneMapping,
        params.outCommandListWasClosed);

    registerEdgeDetectionPasses(
        graph,
        ldrColor,
        ldrColorScratch,
        params,
        params.settings->PostProcessEdgeDetection && params.edgeDetectionPipeline != nullptr);
}

} // namespace caustica::rg

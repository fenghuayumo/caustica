#include <render/graph/FrameGraphBuild.h>

#include <render/core/RenderTargets.h>
#include <render/passes/pathTrace/PathTraceGraph.h>
#include <render/passes/denoisers/NrdGraph.h>
#include <render/passes/composite/BlitGraphPass.h>
#include <render/passes/postProcess/DenoiseAAGraph.h>
#include <render/passes/postProcess/PostProcessGraph.h>

#include <cassert>

namespace caustica::rg
{

void buildFrameGraph(const FrameGraphBuildParams& params)
{
    assert(params.renderTargets);
    assert(params.settings);
    assert(params.compositeView);
    assert(params.toneMappingPass);
    assert(params.targetFramebuffer);
    assert(params.bindingCache);
    assert(params.blitPass);

    PathTraceGraphParams pathTraceParams{
        .graph = params.graph,
        .worldRenderer = params.worldRenderer,
        .renderTargets = params.renderTargets,
        .settings = params.settings,
        .frameContext = params.frameContext,
        .framePassRegistry = params.framePassRegistry,
        .framebuffer = params.targetFramebuffer,
        .sampleConstants = params.sampleConstants,
        .hasScene = params.hasScene,
    };
    buildPathTraceGraph(pathTraceParams);

    NrdGraphParams nrdParams{
        .graph = params.graph,
        .worldRenderer = params.worldRenderer,
        .renderTargets = params.renderTargets,
        .settings = params.settings,
        .framebuffer = params.targetFramebuffer,
        .hasScene = params.hasScene,
    };
    buildNrdGraph(nrdParams);

    DenoiseAAGraphParams denoiseParams{
        .graph = params.graph,
        .renderTargets = params.renderTargets,
        .settings = params.settings,
        .framePassRegistry = params.framePassRegistry,
        .frameContext = params.frameContext,
        .worldRenderer = params.worldRenderer,
        .aaReset = params.aaReset,
        .camera = params.camera,
        .compositeView = params.compositeView,
        .temporalAAPass = params.temporalAAPass,
        .accumulationPass = params.accumulationPass,
        .frameIndex = params.frameIndex,
        .accumulationSampleIndex = params.accumulationSampleIndex,
        .gaussianSplatTemporalSampleIndex = params.gaussianSplatTemporalSampleIndex,
        .gaussianSplatTemporalReset = params.gaussianSplatTemporalReset,
    };
    buildDenoiseAndAAGraph(denoiseParams);

    PostProcessGraphParams postParams{
        .graph = params.graph,
        .renderTargets = params.renderTargets,
        .settings = params.settings,
        .camera = params.camera,
        .bloomPass = params.bloomPass,
        .toneMappingPass = params.toneMappingPass,
        .compositeView = params.compositeView,
        .displaySize = params.displaySize,
        .pathTracingBindingSet = params.pathTracingBindingSet,
        .descriptorTable = params.descriptorTable,
        .testRaygenPpHdrPipeline = params.testRaygenPpHdrPipeline,
        .edgeDetectionPipeline = params.edgeDetectionPipeline,
        .outCommandListWasClosed = params.outCommandListWasClosed,
    };
    buildPostProcessGraph(postParams);

    if (params.framePassRegistry && params.frameContext)
    {
        params.framePassRegistry->applyGraphPasses(
            render::FramePassInsertPoint::AfterToneMapping,
            params.graph,
            *params.frameContext);
    }

    const TextureHandle ldrColor = params.graph.importTexture(
        params.renderTargets->ldrColor,
        nvrhi::ResourceStates::ShaderResource);

    FinalBlitPassParams blitParams{};
    blitParams.sourceLdrColor = ldrColor;
    blitParams.targetFramebuffer = params.targetFramebuffer;
    blitParams.bindingCache = params.bindingCache;
    registerFinalBlitPass(params.graph, blitParams, *params.blitPass);
}

} // namespace caustica::rg

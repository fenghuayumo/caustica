#include <render/FrameGraphPasses.h>

#include <render/FrameGraphContext.h>
#include <render/PathTracingContext.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/lighting/LightingFrame.h>
#include <render/passes/lighting/distant/EnvMapImportanceSamplingCache.h>
#include <render/passes/lighting/distant/EnvMapProcessor.h>
#include <shaders/SampleConstantBuffer.h>

#include <cassert>

namespace caustica::render
{

void registerLightingUpdateBeginPass(FrameGraphContext ctx)
{
    assert(ctx.graph);

    // Always register when a scene is present so PathTrace / RTXDI executeAfter
    // edges resolve even if lighting caches are not ready yet.
    if (!ctx.hasScene)
        return;

    rg::PassOptions passOptions{};
    passOptions.sideEffect = true;
    passOptions.executeAfter = "ClearFrameTargets";

    ctx.graph->addPass(
        "LightingUpdateBegin",
        [](rg::PassBuilder&) {},
        [ctx](rg::RenderPassContext& passCtx) {
            caustica::rhi::ICommandList* commandList = passCtx.commandList();
            if (commandList == nullptr || ctx.pathTracingContext == nullptr
                || ctx.lightSampling == nullptr)
                return;

            updateLightingFrame(
                *ctx.pathTracingContext,
                commandList,
                ctx.frameIndex,
                ctx.gaussianSplatEmissionProxies);

            ctx.pathTracingContext->scenePasses.rayTracing.uploadSubInstanceData(commandList);

            if (ctx.sampleConstants != nullptr && ctx.constantBuffer != nullptr
                && ctx.environment != nullptr)
            {
                ctx.sampleConstants->envMapImportanceSamplingParams =
                    ctx.environment->getImportanceSampling()->getShaderParams();
                commandList->writeBuffer(
                    ctx.constantBuffer,
                    ctx.sampleConstants,
                    sizeof(SampleConstants));
            }
        },
        passOptions);
}

} // namespace caustica::render

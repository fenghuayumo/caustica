#include <render/passes/postProcess/DenoiseAAGraph.h>

#include <render/core/RenderTargets.h>
#include <render/FramePassRegistry.h>
#include <render/worldRenderer/PathTracingFrameContext.h>

#include <cassert>

namespace caustica::rg
{

namespace
{
    bool needsRealtimeCopyPass(const PathTracerSettings& settings)
    {
        return settings.RealtimeMode && settings.RealtimeAA == 0;
    }
}

void buildDenoiseAndAAGraph(const DenoiseAAGraphParams& params)
{
    assert(params.renderTargets);
    assert(params.settings);

    GraphBuilder& graph = params.graph;

    if (needsRealtimeCopyPass(*params.settings))
    {
        const TextureHandle processedOutput = graph.importTexture(
            params.renderTargets->processedOutputColor,
            TextureAccess::CopySource);
        const TextureHandle outputColor = graph.importTexture(
            params.renderTargets->outputColor,
            TextureAccess::CopyDest);
        graph.extractTexture(processedOutput, TextureAccess::UnorderedAccess);
        graph.extractTexture(outputColor, TextureAccess::UnorderedAccess);

        graph.addPass(
            "CopyProcessedToOutput",
            [&](PassBuilder& setup) {
                setup.read(processedOutput, TextureAccess::CopySource);
                setup.write(outputColor, TextureAccess::CopyDest);
            },
            [processedOutput, outputColor](RenderPassContext& ctx) {
                ctx.commandList()->copyTexture(
                    ctx.texture(outputColor),
                    nvrhi::TextureSlice(),
                    ctx.texture(processedOutput),
                    nvrhi::TextureSlice());
            });
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

#include <render/FrameGraphPasses.h>

#include <render/FrameGraphContext.h>
#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <shaders/PathTracer/Config.h>

#include <cassert>
#include <utility>
#include <vector>

namespace caustica::render
{

void registerDebugOverlayGraphPasses(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.targetFramebuffer);
    assert(ctx.renderTargets);

    caustica::rhi::Framebuffer* framebuffer = ctx.targetFramebuffer;
    const auto& fbinfo = framebuffer->getFramebufferInfo();
    const bool showDebugLines = ctx.showDebugLines;
    const bool copyDebugFeedback = ctx.copyDebugFeedback;

    std::vector<DebugLineStruct> cpuSideDebugLines;
    if (ctx.cpuSideDebugLines != nullptr)
    {
        cpuSideDebugLines = std::move(*ctx.cpuSideDebugLines);
        ctx.cpuSideDebugLines->clear();
    }

    rg::BufferHandle debugLineCapture{};
    rg::BufferHandle debugLineDisplay{};

    if (showDebugLines || copyDebugFeedback)
    {
        debugLineCapture = ctx.graph->importBuffer(
            ctx.debugLineBufferCapture,
            caustica::rhi::ResourceStates::Common);
        debugLineDisplay = ctx.graph->importBuffer(
            ctx.debugLineBufferDisplay,
            caustica::rhi::ResourceStates::Common);

        ctx.graph->extractBuffer(debugLineCapture, caustica::rhi::ResourceStates::Common);
        ctx.graph->extractBuffer(debugLineDisplay, caustica::rhi::ResourceStates::Common);
    }

    if (showDebugLines)
    {
        caustica::rhi::Texture* targetColor = framebuffer->getDesc().colorAttachments[0].texture;
        assert(targetColor);

        const rg::TextureHandle targetColorHandle = ctx.graph->importTexture(
            targetColor,
            rg::TextureAccess::RenderTarget);
        const rg::TextureHandle depth = ctx.graph->importTexture(
            ctx.renderTargets->depth,
            rg::TextureAccess::ShaderResource);
        const rg::BufferHandle constantBuffer = ctx.graph->importBuffer(
            ctx.constantBuffer,
            rg::BufferAccess::ConstantBuffer);
        const uint32_t capturedLineVertexCount = ctx.capturedLineVertexCount;
        const uint32_t cpuLineVertexCount = static_cast<uint32_t>(cpuSideDebugLines.size());
        const caustica::rhi::BindingSetHandle linesBindingSet = ctx.linesBindingSet;
        const caustica::rhi::GraphicsPipelineHandle linesPipeline = ctx.linesPipeline;

        if (!cpuSideDebugLines.empty())
        {
            ctx.graph->addPass(
                "UploadCpuDebugLines",
                [debugLineCapture](rg::PassBuilder& setup) {
                    setup.write(debugLineCapture, rg::BufferAccess::CopyDest);
                },
                [debugLineCapture, cpuSideDebugLines = std::move(cpuSideDebugLines)](
                    rg::RenderPassContext& passCtx) {
                    passCtx.commandList()->writeBuffer(
                        passCtx.buffer(debugLineCapture),
                        cpuSideDebugLines.data(),
                        sizeof(DebugLineStruct) * cpuSideDebugLines.size());
                },
                rg::PassOptions{ .sideEffect = true });
        }

        ctx.graph->addPass(
            "DebugLines",
            [targetColorHandle, depth, constantBuffer, debugLineCapture, debugLineDisplay](rg::PassBuilder& setup) {
                setup.write(targetColorHandle, rg::TextureAccess::RenderTarget);
                setup.read(depth, rg::TextureAccess::ShaderResource);
                setup.read(constantBuffer, rg::BufferAccess::ConstantBuffer);
                setup.read(debugLineCapture, rg::BufferAccess::VertexBuffer);
                setup.read(debugLineDisplay, rg::BufferAccess::VertexBuffer);
            },
            [framebuffer, viewport = fbinfo.getViewport(), capturedLineVertexCount,
                cpuLineVertexCount, debugLineCapture, debugLineDisplay,
                linesBindingSet, linesPipeline](rg::RenderPassContext& passCtx) {
                caustica::rhi::CommandList* commandList = passCtx.commandList();
                commandList->beginMarker("Debug Lines");

                caustica::rhi::GraphicsState state;
                state.bindings = { linesBindingSet };
                state.vertexBuffers = { {passCtx.buffer(debugLineDisplay), 0, 0} };
                state.pipeline = linesPipeline;
                state.framebuffer = framebuffer;
                state.viewport.addViewportAndScissorRect(viewport);
                commandList->setGraphicsState(state);

                caustica::rhi::DrawArguments args;
                args.vertexCount = capturedLineVertexCount;
                commandList->draw(args);

                if (cpuLineVertexCount > 0)
                {
                    state.vertexBuffers = { {passCtx.buffer(debugLineCapture), 0, 0} };
                    commandList->setGraphicsState(state);

                    args.vertexCount = cpuLineVertexCount;
                    commandList->draw(args);
                }

                commandList->endMarker();
            },
            rg::PassOptions{ .sideEffect = true, .executeAfter = "Blit" });
    }

    if (copyDebugFeedback)
    {
        const rg::BufferHandle feedbackCpu = ctx.graph->importBuffer(
            ctx.feedbackBufferCpu,
            rg::BufferAccess::CopyDest);
        const rg::BufferHandle feedbackGpu = ctx.graph->importBuffer(
            ctx.feedbackBufferGpu,
            caustica::rhi::ResourceStates::Common);
        const rg::BufferHandle debugDeltaPathTreeCpu = ctx.graph->importBuffer(
            ctx.debugDeltaPathTreeCpu,
            rg::BufferAccess::CopyDest);
        const rg::BufferHandle debugDeltaPathTreeGpu = ctx.graph->importBuffer(
            ctx.debugDeltaPathTreeGpu,
            caustica::rhi::ResourceStates::Common);

        ctx.graph->extractBuffer(feedbackCpu, rg::BufferAccess::CopyDest);
        ctx.graph->extractBuffer(feedbackGpu, caustica::rhi::ResourceStates::Common);
        ctx.graph->extractBuffer(debugDeltaPathTreeCpu, rg::BufferAccess::CopyDest);
        ctx.graph->extractBuffer(debugDeltaPathTreeGpu, caustica::rhi::ResourceStates::Common);

        ctx.graph->addPass(
            "DebugFeedbackCopies",
            [feedbackCpu, feedbackGpu, debugLineCapture, debugLineDisplay,
                debugDeltaPathTreeCpu, debugDeltaPathTreeGpu](rg::PassBuilder& setup) {
                setup.read(feedbackGpu, rg::BufferAccess::CopySource);
                setup.write(feedbackCpu, rg::BufferAccess::CopyDest);
                setup.read(debugLineCapture, rg::BufferAccess::CopySource);
                setup.write(debugLineDisplay, rg::BufferAccess::CopyDest);
                setup.read(debugDeltaPathTreeGpu, rg::BufferAccess::CopySource);
                setup.write(debugDeltaPathTreeCpu, rg::BufferAccess::CopyDest);
            },
            [feedbackCpu, feedbackGpu, debugLineCapture, debugLineDisplay,
                debugDeltaPathTreeCpu, debugDeltaPathTreeGpu](rg::RenderPassContext& passCtx) {
                caustica::rhi::CommandList* commandList = passCtx.commandList();
                commandList->copyBuffer(
                    passCtx.buffer(feedbackCpu), 0,
                    passCtx.buffer(feedbackGpu), 0,
                    sizeof(DebugFeedbackStruct));
                commandList->copyBuffer(
                    passCtx.buffer(debugLineDisplay), 0,
                    passCtx.buffer(debugLineCapture), 0,
                    sizeof(DebugLineStruct) * MAX_DEBUG_LINES);
                commandList->copyBuffer(
                    passCtx.buffer(debugDeltaPathTreeCpu), 0,
                    passCtx.buffer(debugDeltaPathTreeGpu), 0,
                    sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices);
            },
            rg::PassOptions{
                .sideEffect = true,
                .executeAfter = showDebugLines ? "DebugLines" : "Blit",
            });
    }
}

} // namespace caustica::render

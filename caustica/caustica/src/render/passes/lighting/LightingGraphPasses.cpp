#include <render/FrameGraphPasses.h>

#include <render/FrameGraphContext.h>
#include <render/PathTracingContext.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/lighting/LightingFrame.h>
#include <render/passes/lighting/LightSamplingCache.h>
#include <render/passes/lighting/distant/EnvMapImportanceSamplingCache.h>
#include <render/passes/lighting/distant/EnvMapProcessor.h>
#include <render/pipeline/FrameGraphPassNames.h>
#include <shaders/FrameConstantBuffer.h>

#include <cassert>

namespace caustica::render
{

rg::PassHandle registerUploadFrameConstantsPass(FrameGraphContext ctx, rg::PassHandle after)
{
    assert(ctx.graph);
    assert(after.isValid());
    if (!ctx.constantBuffer || !ctx.frameConstants)
        return after;

    const rg::BufferHandle constants =
        ctx.graph->importBuffer(ctx.constantBuffer, rg::BufferAccess::CopyDest);
    const caustica::rhi::BufferHandle constantBuffer = ctx.constantBuffer;
    const FrameConstants frameConstants = *ctx.frameConstants;

    return ctx.graph->addPass(
        kUploadFrameConstantsPass,
        [constants](rg::PassBuilder& setup) {
            setup.write(constants, rg::BufferAccess::CopyDest);
        },
        [constantBuffer, frameConstants](rg::RenderPassContext& passCtx) {
            if (passCtx.commandList() == nullptr || constantBuffer == nullptr)
                return;
            passCtx.commandList()->writeBuffer(
                constantBuffer,
                &frameConstants,
                sizeof(FrameConstants));
        },
        rg::PassOptions{
            .sideEffect = true,
            .queue = caustica::rhi::CommandQueue::Copy,
        });
}

rg::PassHandle registerLightingGraphPasses(FrameGraphContext ctx, rg::PassHandle after)
{
    assert(ctx.graph);
    assert(after.isValid());

    if (!ctx.hasScene)
        return after;

    // EnvMapUpdate → LightSamplingUpdateBegin → UploadSubInstanceData via resource edges.
    rg::PassHandle previous = after;
    {
        rg::TextureHandle envCube{};
        rg::TextureHandle radianceImportance{};
        if (ctx.environment != nullptr)
        {
            if (auto cube = ctx.environment->getEnvMapCube())
                envCube = ctx.graph->importTexture(cube, rg::TextureAccess::UnorderedAccess);
            if (const auto& importance = ctx.environment->getImportanceSampling())
            {
                if (auto map = importance->getRadianceAndImportanceMap())
                    radianceImportance =
                        ctx.graph->importTexture(map, rg::TextureAccess::UnorderedAccess);
            }
        }
        if (envCube.isValid() || radianceImportance.isValid())
        {
            PathTracingContext* const pathTracingContext = ctx.pathTracingContext;
            const uint64_t frameIndex = ctx.frameIndex;

            previous = ctx.graph->addPass(
                kEnvMapUpdatePass,
                [envCube, radianceImportance](rg::PassBuilder& setup) {
                    if (envCube.isValid())
                        setup.write(envCube, rg::TextureAccess::UnorderedAccess);
                    if (radianceImportance.isValid())
                        setup.write(radianceImportance, rg::TextureAccess::UnorderedAccess);
                },
                [pathTracingContext, frameIndex](rg::RenderPassContext& passCtx) {
                    if (passCtx.commandList() == nullptr || pathTracingContext == nullptr)
                        return;
                    updateEnvMapFrame(*pathTracingContext, passCtx.commandList(), frameIndex);
                },
                rg::PassOptions{ .queue = caustica::rhi::CommandQueue::Compute });
        }
    }

    {
        rg::TextureHandle radianceImportance{};
        rg::BufferHandle lightBuffer{};
        rg::BufferHandle lightProxies{};
        if (ctx.environment != nullptr)
        {
            if (const auto& importance = ctx.environment->getImportanceSampling())
            {
                if (auto map = importance->getRadianceAndImportanceMap())
                    radianceImportance =
                        ctx.graph->importTexture(map, rg::TextureAccess::ShaderResource);
            }
        }
        if (ctx.lightSampling != nullptr)
        {
            if (auto buffer = ctx.lightSampling->getLightBuffer())
                lightBuffer = ctx.graph->importBuffer(buffer, rg::BufferAccess::UnorderedAccess);
            if (auto buffer = ctx.lightSampling->getLightSamplingProxies())
                lightProxies = ctx.graph->importBuffer(buffer, rg::BufferAccess::UnorderedAccess);
        }
        PathTracingContext* const pathTracingContext = ctx.pathTracingContext;
        LightSamplingCache* const lightSampling = ctx.lightSampling;
        const uint64_t frameIndex = ctx.frameIndex;
        const std::vector<GaussianSplatEmissionProxy>* const gaussianEmissionProxies =
            ctx.gaussianSplatEmissionProxies;

        if (radianceImportance.isValid() || lightBuffer.isValid() || lightProxies.isValid())
        {
            previous = ctx.graph->addPass(
                kLightSamplingUpdateBeginPass,
                [radianceImportance, lightBuffer, lightProxies](rg::PassBuilder& setup) {
                    if (radianceImportance.isValid())
                        setup.read(radianceImportance, rg::TextureAccess::ShaderResource);
                    if (lightBuffer.isValid())
                        setup.write(lightBuffer, rg::BufferAccess::UnorderedAccess);
                    if (lightProxies.isValid())
                        setup.write(lightProxies, rg::BufferAccess::UnorderedAccess);
                },
                [pathTracingContext, lightSampling, frameIndex,
                 gaussianEmissionProxies](rg::RenderPassContext& passCtx) {
                    if (passCtx.commandList() == nullptr || pathTracingContext == nullptr
                        || lightSampling == nullptr)
                        return;
                    updateLightSamplingBeginFrame(
                        *pathTracingContext,
                        passCtx.commandList(),
                        frameIndex,
                        gaussianEmissionProxies);
                },
                rg::PassOptions{ .queue = caustica::rhi::CommandQueue::Compute });
        }
    }

    {
        rg::BufferHandle subInstance{};
        rg::BufferHandle constants{};
        if (ctx.subInstanceDataBuffer)
            subInstance =
                ctx.graph->importBuffer(ctx.subInstanceDataBuffer, rg::BufferAccess::CopyDest);
        if (ctx.constantBuffer)
            constants = ctx.graph->importBuffer(ctx.constantBuffer, rg::BufferAccess::CopyDest);
        PathTracingContext* const pathTracingContext = ctx.pathTracingContext;
        FrameConstants* const frameConstants = ctx.frameConstants;
        const caustica::rhi::BufferHandle constantBuffer = ctx.constantBuffer;
        EnvMapProcessor* const environment = ctx.environment;

        previous = ctx.graph->addPass(
            kUploadSubInstanceDataPass,
            [subInstance, constants](rg::PassBuilder& setup) {
                if (subInstance.isValid())
                    setup.write(subInstance, rg::BufferAccess::CopyDest);
                if (constants.isValid())
                    setup.write(constants, rg::BufferAccess::CopyDest);
            },
            [pathTracingContext, frameConstants, constantBuffer,
             environment](rg::RenderPassContext& passCtx) {
                caustica::rhi::CommandList* commandList = passCtx.commandList();
                if (commandList == nullptr || pathTracingContext == nullptr)
                    return;

                pathTracingContext->scenePasses.rayTracing.uploadSubInstanceData(commandList);

                if (frameConstants != nullptr && constantBuffer != nullptr
                    && environment != nullptr)
                {
                    frameConstants->envMapImportanceSamplingParams =
                        environment->getImportanceSampling()->getShaderParams();
                    commandList->writeBuffer(
                        constantBuffer,
                        frameConstants,
                        sizeof(FrameConstants));
                }
            },
            rg::PassOptions{ .queue = caustica::rhi::CommandQueue::Copy });
    }

    return previous;
}

} // namespace caustica::render

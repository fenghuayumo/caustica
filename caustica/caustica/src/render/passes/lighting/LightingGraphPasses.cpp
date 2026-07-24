#include <render/FrameGraphPasses.h>

#include <render/FrameGraphContext.h>
#include <render/PathTracingContext.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/lighting/LightingFrame.h>
#include <render/passes/lighting/LightSamplingCache.h>
#include <render/passes/lighting/distant/EnvMapImportanceSamplingCache.h>
#include <render/passes/lighting/distant/EnvMapProcessor.h>
#include <render/pipeline/FrameGraphPassNames.h>
#include <shaders/SampleConstantBuffer.h>

#include <cassert>

namespace caustica::render
{

void registerLightingGraphPasses(FrameGraphContext ctx)
{
    assert(ctx.graph);

    if (!ctx.hasScene)
        return;

    // EnvMapUpdate → LightSamplingUpdateBegin → UploadSubInstanceData
    // Downstream passes depend on kLightingReadyPass (UploadSubInstanceData).

    {
        rg::PassOptions passOptions{};
        passOptions.sideEffect = true;
        passOptions.executeAfter = kClearFrameTargetsPass;

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

        ctx.graph->addPass(
            kEnvMapUpdatePass,
            [envCube, radianceImportance](rg::PassBuilder& setup) {
                if (envCube.isValid())
                    setup.write(envCube, rg::TextureAccess::UnorderedAccess);
                if (radianceImportance.isValid())
                    setup.write(radianceImportance, rg::TextureAccess::UnorderedAccess);
            },
            [ctx](rg::RenderPassContext& passCtx) {
                if (passCtx.commandList() == nullptr || ctx.pathTracingContext == nullptr)
                    return;
                updateEnvMapFrame(*ctx.pathTracingContext, passCtx.commandList(), ctx.frameIndex);
            },
            passOptions);
    }

    {
        rg::PassOptions passOptions{};
        passOptions.sideEffect = true;
        passOptions.executeAfter = kEnvMapUpdatePass;

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

        ctx.graph->addPass(
            kLightSamplingUpdateBeginPass,
            [radianceImportance, lightBuffer, lightProxies](rg::PassBuilder& setup) {
                if (radianceImportance.isValid())
                    setup.read(radianceImportance, rg::TextureAccess::ShaderResource);
                if (lightBuffer.isValid())
                    setup.write(lightBuffer, rg::BufferAccess::UnorderedAccess);
                if (lightProxies.isValid())
                    setup.write(lightProxies, rg::BufferAccess::UnorderedAccess);
            },
            [ctx](rg::RenderPassContext& passCtx) {
                if (passCtx.commandList() == nullptr || ctx.pathTracingContext == nullptr
                    || ctx.lightSampling == nullptr)
                    return;
                updateLightSamplingBeginFrame(
                    *ctx.pathTracingContext,
                    passCtx.commandList(),
                    ctx.frameIndex,
                    ctx.gaussianSplatEmissionProxies);
            },
            passOptions);
    }

    {
        rg::PassOptions passOptions{};
        passOptions.sideEffect = true;
        passOptions.executeAfter = kLightSamplingUpdateBeginPass;

        rg::BufferHandle subInstance{};
        rg::BufferHandle constants{};
        if (ctx.subInstanceDataBuffer)
            subInstance =
                ctx.graph->importBuffer(ctx.subInstanceDataBuffer, rg::BufferAccess::CopyDest);
        if (ctx.constantBuffer)
            constants = ctx.graph->importBuffer(ctx.constantBuffer, rg::BufferAccess::CopyDest);

        ctx.graph->addPass(
            kUploadSubInstanceDataPass,
            [subInstance, constants](rg::PassBuilder& setup) {
                if (subInstance.isValid())
                    setup.write(subInstance, rg::BufferAccess::CopyDest);
                if (constants.isValid())
                    setup.write(constants, rg::BufferAccess::CopyDest);
            },
            [ctx](rg::RenderPassContext& passCtx) {
                caustica::rhi::CommandList* commandList = passCtx.commandList();
                if (commandList == nullptr || ctx.pathTracingContext == nullptr)
                    return;

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
}

void registerLightingUpdateBeginPass(FrameGraphContext ctx)
{
    registerLightingGraphPasses(ctx);
}

} // namespace caustica::render

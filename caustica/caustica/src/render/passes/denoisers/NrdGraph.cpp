#include <render/passes/denoisers/NrdGraph.h>

#include <render/core/RenderTargets.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <shaders/PathTracer/Config.h>
#include <shaders/PathTracer/Config.h>

#include <cassert>
#include <format>

namespace caustica::rg
{

namespace
{
    void declareNrdPlaneAccess(
        PassBuilder& setup,
        GraphBuilder& graph,
        RenderTargets* targets,
        int planeIndex,
        bool readsOutputColor)
    {
        const TextureHandle denoiserViewspaceZ = graph.importTexture(
            targets->denoiserViewspaceZ, TextureAccess::ShaderResource);
        const TextureHandle denoiserMotionVectors = graph.importTexture(
            targets->denoiserMotionVectors, TextureAccess::ShaderResource);
        const TextureHandle denoiserNormalRoughness = graph.importTexture(
            targets->denoiserNormalRoughness, TextureAccess::ShaderResource);
        const TextureHandle denoiserDiffRadianceHitDist = graph.importTexture(
            targets->denoiserDiffRadianceHitDist, TextureAccess::ShaderResource);
        const TextureHandle denoiserSpecRadianceHitDist = graph.importTexture(
            targets->denoiserSpecRadianceHitDist, TextureAccess::ShaderResource);
        const TextureHandle denoiserDisocclusionThresholdMix = graph.importTexture(
            targets->denoiserDisocclusionThresholdMix, TextureAccess::ShaderResource);
        const TextureHandle outputColor = graph.importTexture(
            targets->outputColor, TextureAccess::UnorderedAccess);
        const TextureHandle outDiff = graph.importTexture(
            targets->denoiserOutDiffRadianceHitDist[planeIndex], TextureAccess::UnorderedAccess);
        const TextureHandle outSpec = graph.importTexture(
            targets->denoiserOutSpecRadianceHitDist[planeIndex], TextureAccess::UnorderedAccess);

        graph.extractTexture(outputColor, TextureAccess::UnorderedAccess);
        graph.extractTexture(outDiff, TextureAccess::UnorderedAccess);
        graph.extractTexture(outSpec, TextureAccess::UnorderedAccess);

        setup.read(denoiserViewspaceZ, TextureAccess::ShaderResource);
        setup.read(denoiserMotionVectors, TextureAccess::ShaderResource);
        setup.read(denoiserNormalRoughness, TextureAccess::ShaderResource);
        setup.read(denoiserDiffRadianceHitDist, TextureAccess::ShaderResource);
        setup.read(denoiserSpecRadianceHitDist, TextureAccess::ShaderResource);
        setup.read(denoiserDisocclusionThresholdMix, TextureAccess::ShaderResource);
        if (readsOutputColor)
            setup.read(outputColor, TextureAccess::ShaderResource);
        setup.write(outDiff, TextureAccess::UnorderedAccess);
        setup.write(outSpec, TextureAccess::UnorderedAccess);
        setup.write(outputColor, TextureAccess::UnorderedAccess);

        if (targets->denoiserOutValidation != nullptr)
        {
            const TextureHandle validation = graph.importTexture(
                targets->denoiserOutValidation, TextureAccess::UnorderedAccess);
            setup.write(validation, TextureAccess::UnorderedAccess);
        }
    }
}

void buildNrdGraph(const NrdGraphParams& params)
{
    assert(params.worldRenderer);
    assert(params.renderTargets);
    assert(params.settings);

    if (!params.hasScene || !params.settings->ActualUseStandaloneDenoiser())
        return;

    params.worldRenderer->ensureNrdIntegrations();

    GraphBuilder& graph = params.graph;
    const int maxPassCount = std::min(
        params.settings->StablePlanesActiveCount,
        static_cast<int>(cStablePlaneCount));

    for (int pass = maxPassCount - 1; pass >= 0; --pass)
    {
        const int planeIndex = pass;
        const bool readsOutputColor = planeIndex < (maxPassCount - 1);
        graph.addPass(
            std::format("NRD Plane {}", planeIndex),
            [params, planeIndex, readsOutputColor](PassBuilder& setup) {
                declareNrdPlaneAccess(setup, params.graph, params.renderTargets, planeIndex, readsOutputColor);
            },
            [params, planeIndex](RenderPassContext& ctx) {
                params.worldRenderer->denoiseStablePlane(
                    ctx.commandList(),
                    params.framebuffer,
                    planeIndex);
            });
    }
}

} // namespace caustica::rg

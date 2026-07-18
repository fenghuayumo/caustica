#include <render/WorldRenderer.h>

#include <render/PathTracingContext.h>
#include <render/passes/gaussian/GaussianSplatFramePass.h>
#include <render/passes/gaussian/GaussianSplatSceneRuntime.h>

using namespace caustica::render;

void caustica::render::WorldRenderer::createGaussianTemporalRenderPasses()
{
    if (!m_gaussianFramePass)
        m_gaussianFramePass = std::make_unique<GaussianSplatFramePass>();

    m_gaussianFramePass->createTemporalResources(
        device(),
        m_context->shaderFactory,
        m_renderTargets.get());
    m_gaussianSplatTemporalReset = true;
}

void caustica::render::WorldRenderer::prepareGaussianSplatPasses()
{
    if (!m_gaussianFramePass)
        m_gaussianFramePass = std::make_unique<GaussianSplatFramePass>();

    bindPassFrameResources();
    m_gaussianFramePass->prepareScenePasses(m_shaderDebug);
}

void caustica::render::WorldRenderer::buildGaussianSplatEmissionProxies()
{
    caustica::render::buildGaussianSplatEmissionProxies(
        m_gaussianSplatEmissionProxies,
        m_context->frameGaussianSplats(),
        m_context->scenePasses.gaussianSplats,
        m_context->activeSettings());
}

#include <render/WorldRenderer.h>

#include <render/PathTracingContext.h>
#include <render/passes/denoisers/DenoisePass.h>

using namespace caustica::render;

void caustica::render::WorldRenderer::createDenoiserRenderPasses()
{
    if (!m_denoisePass)
        m_denoisePass = std::make_unique<DenoisePass>();

    m_denoisePass->createGuides(
        device(),
        m_context->shaderFactory,
        m_renderTargets,
        m_shaderDebug,
        m_bindingLayout);
}

void caustica::render::WorldRenderer::bindPassFrameResources()
{
    if (m_denoisePass)
    {
        DenoisePass::FrameBindings bindings{};
        bindings.context = m_context;
        bindings.device = device();
        bindings.renderTargets = m_renderTargets.get();
        bindings.postProcess = m_postProcess.get();
        bindings.bindingSet = m_bindingSet;
        bindings.bindingLayout = m_bindingLayout;
        bindings.constantBuffer = m_constantBuffer;
        bindings.commandList = m_commandList;
        bindings.renderSize = m_renderSize;
        bindings.displaySize = m_displaySize;
        bindings.displayAspectRatio = m_displayAspectRatio;
        bindings.cameraJitter = computeCameraJitter();
        bindings.sampleIndex = m_sampleIndex;
        bindings.frameIndex = m_frameIndex;
        bindings.accumulationSampleIndex = m_accumulationSampleIndex;
        bindings.accumulationCompleted = m_accumulationCompleted;
        bindings.gaussianSplatTemporalSampleIndex = &m_gaussianSplatTemporalSampleIndex;
        bindings.gaussianSplatTemporalReset = &m_frameGaussianSplatTemporalReset;
        bindings.temporalAntiAliasing = m_temporalAntiAliasingPass.get();
        bindings.accumulation = m_accumulationPass.get();
        bindings.camera = &m_context->camera;
#if CAUSTICA_WITH_STREAMLINE
        bindings.dlssRROptions = &m_lastDLSSRROptions;
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
        bindings.nativeDLSS = m_nativeDLSS.get();
#endif
        m_denoisePass->bindFrame(bindings);
    }

    if (m_gaussianFramePass)
    {
        GaussianSplatFramePass::FrameBindings bindings{};
        bindings.context = m_context;
        bindings.device = device();
        bindings.renderTargets = m_renderTargets.get();
        bindings.accelStructs = &m_accelStructs;
        bindings.scenePasses = &m_context->scenePasses.gaussianSplats;
        bindings.displaySize = m_displaySize;
        bindings.frameIndex = m_frameIndex;
        bindings.sampleIndex = m_sampleIndex;
        bindings.temporalSampleIndex = &m_gaussianSplatTemporalSampleIndex;
        bindings.frameTemporalReset = &m_frameGaussianSplatTemporalReset;
        bindings.temporalReset = &m_gaussianSplatTemporalReset;
        m_gaussianFramePass->bindFrame(bindings);
    }
}

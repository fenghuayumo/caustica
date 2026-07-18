#include <render/WorldRenderer.h>

#include <render/PathTracingContext.h>
#include <render/passes/rtxdi/RtxdiPass.h>
#include <render/passes/gaussian/GaussianSplatGraph.h>
#include <render/passes/gaussian/GaussianSplatEmissionProxy.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <render/core/AccelStructManager.h>
#include <render/core/PathTracerSettings.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>

using namespace caustica;
using namespace caustica::math;
using namespace caustica::render;

void caustica::render::WorldRenderer::createRtxdiRenderPasses()
{
    if (m_context->activeSettings().actualUseRTXDIPasses())
        m_rtxdiPass = std::make_unique<RtxdiPass>(device(), m_context->shaderFactory, m_context->renderDevice, m_bindlessLayout);
    else
        m_rtxdiPass = nullptr;
}

void caustica::render::WorldRenderer::rtxdiSetupFrame(nvrhi::IFramebuffer* framebuffer, PathTracerCameraData cameraData, uint2 renderDims)
{
    const bool envMapPresent = m_context->activeSettings().EnvironmentMapParams.enabled;

    RtxdiBridgeParameters bridgeParameters;
	bridgeParameters.frameIndex = m_frameIndex & 0xFFFFFFFF;
	bridgeParameters.frameDims = renderDims;
	bridgeParameters.cameraPosition = m_context->camera.camera().getPosition();
	bridgeParameters.userSettings = m_context->activeSettings().RTXDI;
    bridgeParameters.usingLightSampling = m_context->activeSettings().actualUseReSTIRDI();
    bridgeParameters.usingReGIR = m_context->activeSettings().actualUseReSTIRDI();

    bridgeParameters.userSettings.restirDI.initialSamplingParams.environmentMapImportanceSampling = envMapPresent;

    buildGaussianSplatEmissionProxies();
    if (!m_gaussianSplatEmissionProxies.empty() && isGaussianSplatEmissionEnabled(m_context->activeSettings()))
    {
        bridgeParameters.gaussianSplatEmissionProxies = &m_gaussianSplatEmissionProxies;
        bridgeParameters.gaussianSplatEmissionObjectToWorld = float4x4::identity();
        bridgeParameters.gaussianSplatEmissionIntensity = m_context->activeSettings().GaussianSplatEmissionIntensity;
    }

    if( m_context->activeSettings().ResetRealtimeCaches )
        m_rtxdiPass->reset();

    const scene::SceneRenderData* renderData = m_context->frameScene;
    size_t geometryInstanceCount = 0;
    if (renderData)
    {
        for (const scene::MeshInstanceRenderProxy& proxy : renderData->meshInstances)
        {
            geometryInstanceCount += proxy.geometryCount;
        }
    }
    nvrhi::IDescriptorTable* descriptorTable = m_context->descriptorTable
        ? m_context->descriptorTable->getDescriptorTable()
        : nullptr;

    m_rtxdiPass->prepareResources(
        m_commandList,
        *m_renderTargets,
        envMapPresent ? m_context->scenePasses.lighting.environment() : nullptr,
        m_context->scenePasses.lighting.envMapSceneParams(),
        renderData,
        geometryInstanceCount,
        descriptorTable,
        m_context->resolveGpuHandles(),
        m_context->scenePasses.lighting.materials(),
        m_context->scenePasses.lighting.opacityMaps(),
        m_context->accelStructs.getSubInstanceBuffer(),
        bridgeParameters,
        m_bindingLayout,
        m_shaderDebug);
}


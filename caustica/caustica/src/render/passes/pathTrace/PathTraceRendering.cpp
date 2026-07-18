#include <render/WorldRenderer.h>

#include <render/PathTracingContext.h>
#include <render/SceneRayTracingResources.h>
#include <render/core/LightingUpdate.h>
#include <render/core/PathTracerSettings.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <render/passes/debug/ShaderDebug.h>
#include <render/passes/postProcess/PostProcess.h>
#include <render/passes/rtxdi/RtxdiPass.h>
#include <assets/loader/ShaderFactory.h>
#include <core/scope.h>
#include <shaders/SampleConstantBuffer.h>
#include <shaders/PathTracer/Config.h>
#include <shaders/PathTracer/StablePlanes.hlsli>
#include <scene/View.h>

#include <algorithm>
#include <chrono>

using namespace caustica;
using namespace caustica::math;
using namespace caustica::render;

bool caustica::render::WorldRenderer::createPTPipeline()
{
    std::vector<caustica::ShaderMacro> shaderMacros;
    m_exportVBufferCS = m_context->shaderFactory->createShader(
        "caustica/shaders/render/processingPasses/ExportVisibilityBuffer.hlsl", "main", &shaderMacros, nvrhi::ShaderType::Compute);
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { m_bindingLayout, m_bindlessLayout };
    pipelineDesc.CS = m_exportVBufferCS;
    m_exportVBufferPSO = device()->createComputePipeline(pipelineDesc);
    return true;
}

void caustica::render::WorldRenderer::updatePathTracerConstants( PathTracerConstants & constants, const PathTracerCameraData & cameraData )
{
#if CAUSTICA_STOCHASTIC_TEXTURE_FILTERING_ENABLE
    auto GetStfMagnificationMethod = [](StfMagnificationMethod method)->int {
        switch (method)
        {
        case StfMagnificationMethod::Default:   return STF_MAGNIFICATION_METHOD_NONE;
        case StfMagnificationMethod::Quad2x2:   return STF_MAGNIFICATION_METHOD_2x2_QUAD;
        case StfMagnificationMethod::Fine2x2:   return STF_MAGNIFICATION_METHOD_2x2_FINE;
        case StfMagnificationMethod::FineTemporal2x2: return STF_MAGNIFICATION_METHOD_2x2_FINE_TEMPORAL;
        case StfMagnificationMethod::FineAlu3x3: return STF_MAGNIFICATION_METHOD_3x3_FINE_ALU;
        case StfMagnificationMethod::FineLut3x3: return STF_MAGNIFICATION_METHOD_3x3_FINE_LUT;
        case StfMagnificationMethod::Fine4x4:    return STF_MAGNIFICATION_METHOD_4x4_FINE;
        default:
            assert(!"Not Implemented");
            return 0;
        }
    };

    auto GetStfFilterMode = [](StfFilterMode mode)->int {
        switch (mode)
        {
        case StfFilterMode::Point:      return STF_FILTER_TYPE_POINT;
        case StfFilterMode::Linear:     return STF_FILTER_TYPE_LINEAR;
        case StfFilterMode::Cubic:      return STF_FILTER_TYPE_CUBIC;
        case StfFilterMode::Gaussian:   return STF_FILTER_TYPE_GAUSSIAN;
        default:
            assert(!"Not Implemented");
            return 0;
        }
    };
#endif // CAUSTICA_STOCHASTIC_TEXTURE_FILTERING_ENABLE

    constants.camera = cameraData;
    constants.prevCamera = cameraData;
    if (m_context->camera.viewPrevious())
        constants.prevCamera.PosW = m_context->camera.viewPrevious()->getInverseViewMatrix().m_translation;

    constants.bounceCount = m_context->activeSettings().BounceCount;
    constants.diffuseBounceCount = m_context->activeSettings().DiffuseBounceCount;
    constants.perPixelJitterAAScale = (m_context->activeSettings().RealtimeMode == false && m_context->activeSettings().AccumulationAA)?(1):( (m_context->activeSettings().RealtimeMode && m_context->activeSettings().RealtimeAA == 3)?(m_context->activeSettings().DLSSRRMicroJitter):(0.0f) );

    // needed to allow super-resolution to work best
    float dlssBias = -dm::log2f(sqrtf((m_displaySize.x * m_displaySize.y) / float(m_renderSize.x * m_renderSize.y)));

    constants.texLODBias = m_context->activeSettings().TexLODBias + dlssBias;
    constants.sampleBaseIndex = m_sampleIndex * m_context->activeSettings().actualSamplesPerPixel();

    //constants.subSampleCount = m_context->activeSettings().actualSamplesPerPixel();
    constants.invSubSampleCount = 1.0f / (float)m_context->activeSettings().actualSamplesPerPixel();

    constants.imageWidth = m_renderSize.x;
    constants.imageHeight = m_renderSize.y;
    if (m_renderTargets != nullptr)
    {
        assert(m_renderSize.x == m_renderTargets->outputColor->getDesc().width);
        assert(m_renderSize.y == m_renderTargets->outputColor->getDesc().height);
    }

    if (m_context->activeSettings().EnableToneMapping && m_toneMappingPass != nullptr)
        constants.preExposedGrayLuminance = dm::luminance(m_toneMappingPass->getPreExposedGray(0));
    else
        constants.preExposedGrayLuminance = 1.0f;

    const float disabledFF = 0.0f;
    if (m_context->activeSettings().RealtimeMode)
        constants.fireflyFilterThreshold = (m_context->activeSettings().RealtimeFireflyFilterEnabled)?(m_context->activeSettings().RealtimeFireflyFilterThreshold*sqrtf(constants.preExposedGrayLuminance)*1e3f):(disabledFF); // it does make sense to make the realtime variant dependent on avg luminance - just didn't have time to try it out yet
    else
        constants.fireflyFilterThreshold = (m_context->activeSettings().ReferenceFireflyFilterEnabled)?(m_context->activeSettings().ReferenceFireflyFilterThreshold*sqrtf(constants.preExposedGrayLuminance)*1e3f):(disabledFF); // making it exposure-adaptive breaks determinism with accumulation (because there's a feedback loop), so that's disabled
    constants.useReSTIRDI = m_context->activeSettings().actualUseReSTIRDI();
    constants.useReSTIRGI = m_context->activeSettings().actualUseReSTIRGI();
    constants.useReSTIRPT = m_context->activeSettings().actualUseReSTIRPT();
    constants.environmentMapVisibleToCamera = m_context->activeSettings().EnvironmentMapParams.VisibleToCamera ? 1u : 0u;
    constants.denoiserRadianceClampK = m_context->activeSettings().DenoiserRadianceClampK;
    constants.DLSSRRBrightnessClampK = (m_context->activeSettings().DLSSRRBrightnessClampK>0)?(m_context->activeSettings().DLSSRRBrightnessClampK * constants.preExposedGrayLuminance):(0.0f);

    // no stable planes by default
    constants.denoisingEnabled = m_context->activeSettings().actualUseStandaloneDenoiser() || m_context->activeSettings().RealtimeAA == 3;

    constants._activeStablePlaneCount           = m_context->activeSettings().StablePlanesActiveCount;
    constants.maxStablePlaneVertexDepth         = std::min( std::min( (uint)m_context->activeSettings().StablePlanesMaxVertexDepth, cStablePlaneMaxVertexIndex ), (uint)m_context->activeSettings().BounceCount );
    constants.allowPrimarySurfaceReplacement    = m_context->activeSettings().AllowPrimarySurfaceReplacement;
    constants.stablePlanesSplitStopThreshold    = m_context->activeSettings().StablePlanesSplitStopThreshold;
    constants._padding3                         = 0;
    constants.stablePlanesSuppressPrimaryIndirectSpecularK  = m_context->activeSettings().StablePlanesSuppressPrimaryIndirectSpecular?m_context->activeSettings().StablePlanesSuppressPrimaryIndirectSpecularK:0.0f;
    constants.stablePlanesAntiAliasingFallthrough = m_context->activeSettings().StablePlanesAntiAliasingFallthrough;
    constants.frameIndex                        = m_frameIndex & 0xFFFFFFFF; //m_context->gpuDevice.getFrameIndex();
    constants.genericTSLineStride               = GenericTSComputeLineStride(constants.imageWidth, constants.imageHeight);
    constants.genericTSPlaneStride              = GenericTSComputePlaneStride(constants.imageWidth, constants.imageHeight);

    constants.NEEEnabled                        = m_context->activeSettings().UseNEE;
    constants.NEEType                           = m_context->activeSettings().NEEType;
    constants.NEECandidateSamples               = m_context->activeSettings().NEECandidateSamples;
    constants.NEEFullSamples                    = m_context->activeSettings().NEEFullSamples;

    constants.EnvironmentMapDiffuseSampleMIPLevel = m_context->activeSettings().EnvironmentMapDiffuseSampleMIPLevel;

#if CAUSTICA_STOCHASTIC_TEXTURE_FILTERING_ENABLE
    // stochastic texture filtering type and size.
    // constants.STFUseBlueNoise                   = m_context->activeSettings().STFUseBlueNoise;
    constants.STFMagnificationMethod            = GetStfMagnificationMethod(m_context->activeSettings().STFMagnificationMethod);
    constants.STFFilterMode                     = GetStfFilterMode(m_context->activeSettings().STFFilterMode);
    constants.STFGaussianSigma                  = m_context->activeSettings().STFGaussianSigma;
#endif
}

void caustica::render::WorldRenderer::pathTracePrePass(nvrhi::ICommandList* commandList)
{
    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    if (!m_ptPipelineBuildStablePlanes || !m_ptPipelineFillStablePlanes)
    {
        m_context->scenePasses.rayTracing.ensureStablePlanePipelines();
        assert(m_ptPipelineBuildStablePlanes && m_ptPipelineFillStablePlanes);
    }

    nvrhi::rt::State state;
    nvrhi::rt::DispatchRaysArguments args;
    const ViewportDesc viewport = m_context->camera.view()->getViewport();
    const uint32_t width = static_cast<uint32_t>(viewport.width());
    const uint32_t height = static_cast<uint32_t>(viewport.height());
    args.width = width;
    args.height = height;

    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    RAII_SCOPE(m_commandList->beginMarker("PathTracePrePass"); , m_commandList->endMarker(); );

    state.shaderTable = m_ptPipelineBuildStablePlanes->getShaderTable();
    state.bindings = { m_bindingSet, m_context->descriptorTable->getDescriptorTable() };
    m_commandList->setRayTracingState(state);
    m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
    m_commandList->dispatchRays(args);

    m_commandList = savedCommandList;
}

void caustica::render::WorldRenderer::vBufferExport(nvrhi::ICommandList* commandList)
{
    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    const ViewportDesc viewport = m_context->camera.view()->getViewport();
    const uint32_t width = static_cast<uint32_t>(viewport.width());
    const uint32_t height = static_cast<uint32_t>(viewport.height());

    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    RAII_SCOPE(m_commandList->beginMarker("VBufferExport"); , m_commandList->endMarker(); );

    nvrhi::ComputeState state;
    state.bindings = { m_bindingSet, m_context->descriptorTable->getDescriptorTable() };
    state.pipeline = m_exportVBufferPSO;
    m_commandList->setComputeState(state);

    const dm::uint2 dispatchSize = {
        (width + NUM_COMPUTE_THREADS_PER_DIM - 1) / NUM_COMPUTE_THREADS_PER_DIM,
        (height + NUM_COMPUTE_THREADS_PER_DIM - 1) / NUM_COMPUTE_THREADS_PER_DIM };
    m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
    m_commandList->dispatch(dispatchSize.x, dispatchSize.y);

    m_commandList = savedCommandList;
}

void caustica::render::WorldRenderer::pathTraceLightingEndUpdate(nvrhi::ICommandList* commandList)
{
    UpdateLightingEndParams lightingEndParams{
        .commandList = commandList,
        .lightSampling = m_context->scenePasses.lighting.lightSampling().get(),
        .bindingCache = &m_context->bindingCache,
        .gpuHandles = m_context->resolveGpuHandles(),
        .materials = m_context->scenePasses.lighting.materials(),
        .opacityMaps = m_context->scenePasses.lighting.opacityMaps(),
        .subInstanceDataBuffer = m_context->accelStructs.getSubInstanceBuffer(),
        .depthBuffer = m_renderTargets->depth,
        .motionVectors = m_renderTargets->screenMotionVectors,
    };
    caustica::updateLightingEnd(lightingEndParams);
}

void caustica::render::WorldRenderer::mainPathTrace(nvrhi::ICommandList* commandList)
{
    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    const bool useStablePlanes = m_context->activeSettings().RealtimeMode;

    nvrhi::rt::State state;
    nvrhi::rt::DispatchRaysArguments args;
    const ViewportDesc viewport = m_context->camera.view()->getViewport();
    const uint32_t width = static_cast<uint32_t>(viewport.width());
    const uint32_t height = static_cast<uint32_t>(viewport.height());
    args.width = width;
    args.height = height;

    RAII_SCOPE(m_commandList->beginMarker("PathTrace");, m_commandList->endMarker(); );

    state.shaderTable = (useStablePlanes ? m_ptPipelineFillStablePlanes : m_ptPipelineReference)->getShaderTable();
    state.bindings = { m_bindingSet, m_context->descriptorTable->getDescriptorTable() };

    for (uint subSampleIndex = 0; subSampleIndex < m_context->activeSettings().actualSamplesPerPixel(); subSampleIndex++)
    {
        m_commandList->setRayTracingState(state);

        SampleMiniConstants miniConstants = { uint4(subSampleIndex, 0, 0, 0) };
        m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));

        m_commandList->dispatchRays(args);
    }

    m_commandList = savedCommandList;
}

void caustica::render::WorldRenderer::preUpdatePathTracing( bool resetAccum, nvrhi::CommandListHandle commandList )
{
    const bool resetReferenceOidn = !m_context->activeSettings().RealtimeMode && (resetAccum || m_context->activeSettings().ResetAccumulation || m_context->activeSettings().ReferenceOIDNDenoiserChanged);
    if (resetReferenceOidn || m_context->activeSettings().ReferenceOIDNDenoiserChanged)
    {
        resetReferenceOIDN();
        m_context->activeSettings().ReferenceOIDNDenoiserChanged = false;
    }

    resetAccum |= m_context->activeSettings().ResetAccumulation;
    resetAccum |= m_context->activeSettings().RealtimeMode;

    if (resetAccum)
    {
        m_accumulationSampleIndex = (m_context->activeSettings().AccumulationPreWarmRealtimeCaches)?(-32):(0);
    }
#if ENABLE_DEBUG_VIZUALISATIONS
    if (resetAccum && m_shaderDebug)
        m_shaderDebug->clearDebugVizTexture(commandList);
#endif

    // profile perf - only makes sense with high accumulation sample counts; only start counting after n-th after it stabilizes
    if( m_accumulationSampleIndex < 16 )
    {
        m_context->diagnostics.benchStart = std::chrono::high_resolution_clock::now( );
        m_context->diagnostics.benchLast = m_context->diagnostics.benchStart;
        m_context->diagnostics.benchFrames = 0;
    } else if( m_accumulationSampleIndex < m_context->activeSettings().AccumulationTarget )
    {
        m_context->diagnostics.benchFrames++;
        m_context->diagnostics.benchLast = std::chrono::high_resolution_clock::now( );
    }
    m_accumulationCompleted = false;
    // 'min' in non-realtime path here is to keep looping the last sample for debugging purposes!
    if( !m_context->activeSettings().RealtimeMode )
    {
        m_sampleIndex = (m_accumulationSampleIndex<0)?(m_accumulationSampleIndex+4096):(min(m_accumulationSampleIndex, m_context->activeSettings().AccumulationTarget - 1));
        m_accumulationCompleted |= m_accumulationSampleIndex == m_context->activeSettings().AccumulationTarget - 1;
    }
    else
        m_sampleIndex = (!m_context->activeSettings().DbgFreezeRealtimeNoiseSeed)?( m_frameIndex % 8192 ):0;     // actual sample index
}

void caustica::render::WorldRenderer::postUpdatePathTracing( )
{
    m_accumulationSampleIndex = std::min( m_accumulationSampleIndex+1, m_context->activeSettings().AccumulationTarget );

    if (m_context->activeSettings().actualUseRTXDIPasses())
        m_rtxdiPass->endFrame();

    m_context->activeSettings().ResetAccumulation = false;
    m_context->activeSettings().ResetRealtimeCaches = false;
    m_frameIndex++;
}

void caustica::render::WorldRenderer::stablePlanesDebugViz(nvrhi::ICommandList* commandList)
{
    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    m_commandList->beginMarker("StablePlanesDebugViz");
    nvrhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();
    m_postProcess->apply(
        m_commandList,
        PostProcess::ComputePassType::StablePlanesDebugViz,
        m_constantBuffer,
        miniConstants,
        m_bindingSet,
        m_bindingLayout,
        tdesc.width,
        tdesc.height);
    m_commandList->endMarker();

    m_commandList = savedCommandList;
}


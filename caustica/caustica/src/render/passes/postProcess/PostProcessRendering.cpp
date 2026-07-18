#include <render/WorldRenderer.h>

#include <render/PathTracingContext.h>
#include <render/core/FramebufferFactory.h>
#include <render/core/PostProcessAA.h>
#include <render/core/PathTracerSettings.h>
#include <render/passes/geometry/BloomPass.h>
#include <render/passes/geometry/TemporalAntiAliasingPass.h>
#include <render/passes/postProcess/AccumulationPass.h>
#include <render/passes/postProcess/PostProcess.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <backend/GpuDevice.h>
#include <core/log.h>
#include <core/scope.h>
#include <scene/View.h>
#include <shaders/SampleConstantBuffer.h>

#include <algorithm>
#include <cmath>

#if CAUSTICA_WITH_STREAMLINE
#include <backend/StreamlineInterface.h>
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
#include <render/passes/geometry/DLSS.h>
#endif

using namespace caustica;
using namespace caustica::math;
using namespace caustica::render;

void caustica::render::WorldRenderer::createAccumulationRenderPasses()
{
    m_accumulationPass = std::make_unique<AccumulationPass>(device(), m_context->shaderFactory);
    m_accumulationPass->createPipeline();
    m_accumulationPass->createBindingSet(m_renderTargets->outputColor, m_renderTargets->accumulatedRadiance, m_renderTargets->processedOutputColor);
}

void caustica::render::WorldRenderer::createPostProcessRenderPasses()
{
    // these get re-created every time intentionally, to pick up changes after at-runtime shader recompile
    m_toneMappingPass = std::make_unique<ToneMappingPass>(device(), m_context->shaderFactory, m_context->renderDevice, m_renderTargets->ldrFramebuffer, *m_context->camera.view(), m_renderTargets->outputColor);
    m_bloomPass = std::make_unique<BloomPass>(device(), m_context->shaderFactory, m_context->renderDevice, m_renderTargets->processedOutputFramebuffer, *m_context->camera.view());
    m_postProcess = std::make_shared<PostProcess>(device(), m_context->shaderFactory, m_context->renderDevice, m_shaderDebug);

    {
        TemporalAntiAliasingPass::CreateParameters taaParams;
        taaParams.sourceDepth = m_renderTargets->depth;
        taaParams.motionVectors = m_renderTargets->screenMotionVectors;
        taaParams.unresolvedColor = m_renderTargets->outputColor;
        taaParams.resolvedColor = m_renderTargets->processedOutputColor;
        taaParams.feedback1 = m_renderTargets->temporalFeedback1;
        taaParams.feedback2 = m_renderTargets->temporalFeedback2;
        taaParams.historyClampRelax = m_renderTargets->combinedHistoryClampRelax;
        taaParams.motionVectorStencilMask = 0; ///*uint32_t motionVectorStencilMask =*/ 0x01;
        taaParams.useCatmullRomFilter = true;

        m_temporalAntiAliasingPass = std::make_unique<TemporalAntiAliasingPass>(device(), m_context->shaderFactory, m_context->renderDevice, *m_context->camera.view(), taaParams);
    }
}

#if CAUSTICA_WITH_NATIVE_DLSS
namespace
{
float GetNativeDLSSResolutionScale(SI::DLSSMode mode)
{
    switch (mode)
    {
    case SI::DLSSMode::eUltraPerformance: return 1.0f / 3.0f;
    case SI::DLSSMode::eMaxPerformance:   return 0.5f;
    case SI::DLSSMode::eBalanced:         return 0.58f;
    case SI::DLSSMode::eMaxQuality:       return 2.0f / 3.0f;
    case SI::DLSSMode::eUltraQuality:     return 0.77f;
    case SI::DLSSMode::eDLAA:             return 1.0f;
    default:                              return 0.58f;
    }
}

uint2 GetNativeDLSSRenderSize(uint2 displaySize, SI::DLSSMode mode)
{
    const float scale = GetNativeDLSSResolutionScale(mode);
    return uint2(
        std::max(1u, uint32_t(std::round(float(displaySize.x) * scale))),
        std::max(1u, uint32_t(std::round(float(displaySize.y) * scale))));
}
}
#endif

#if CAUSTICA_WITH_STREAMLINE
void caustica::render::WorldRenderer::streamlinePreRender()
{
#if CAUSTICA_WITH_STREAMLINE
    if (m_context->gpuDevice.isHeadless())
        return;

    auto& streamline = m_context->gpuDevice.getStreamline();
    m_context->activeSettings().IsDLSSSuported = streamline.isDLSSAvailable();
    m_context->activeSettings().IsDLSSRRSupported = streamline.isDLSSRRAvailable();
    m_context->activeSettings().IsDLSSFGSupported = streamline.isDLSSGAvailable();
    m_context->activeSettings().IsReflexSupported = streamline.isReflexAvailable();

    // Setup Reflex
    {
        auto reflexConsts = caustica::StreamlineInterface::ReflexOptions{};
        reflexConsts.mode = (caustica::StreamlineInterface::ReflexMode) m_context->activeSettings().actualReflexMode();
        reflexConsts.frameLimitUs = m_context->activeSettings().ReflexCappedFps == 0 ? 0 : int(1000000. / m_context->activeSettings().ReflexCappedFps);
        reflexConsts.useMarkersToOptimize = true;
        reflexConsts.virtualKey = VK_F13;
        reflexConsts.idThread = 0; // std::hash<std::thread::id>()(std::this_thread::get_id())
        streamline.setReflexConsts(reflexConsts);

        // Need to update StreamlineIntegration with the ability to query reflex state
        caustica::StreamlineInterface::ReflexState reflexState{};
        streamline.getReflexState(reflexState);
        if (m_context->activeSettings().IsReflexSupported)
        {
            m_context->activeSettings().IsReflexLowLatencyAvailable = reflexState.lowLatencyAvailable;
            m_context->activeSettings().IsReflexFlashIndicatorDriverControlled = reflexState.flashIndicatorDriverControlled;

            auto report = reflexState.frameReport[63];
            if (reflexState.lowLatencyAvailable && report.gpuRenderEndTime != 0)
            {
                auto frameID = report.frameID;
                auto totalGameToRenderLatencyUs = report.gpuRenderEndTime - report.inputSampleTime;
                auto simDeltaUs = report.simEndTime - report.simStartTime;
                auto renderDeltaUs = report.renderSubmitEndTime - report.renderSubmitStartTime;
                auto presentDeltaUs = report.presentEndTime - report.presentStartTime;
                auto driverDeltaUs = report.driverEndTime - report.driverStartTime;
                auto osRenderQueueDeltaUs = report.osRenderQueueEndTime - report.osRenderQueueStartTime;
                auto gpuRenderDeltaUs = report.gpuRenderEndTime - report.gpuRenderStartTime;

                m_context->activeSettings().ReflexStats = "frameID: " + std::to_string(frameID);
                m_context->activeSettings().ReflexStats += "\ntotalGameToRenderLatencyUs: " + std::to_string(totalGameToRenderLatencyUs);
                m_context->activeSettings().ReflexStats += "\nsimDeltaUs: " + std::to_string(simDeltaUs);
                m_context->activeSettings().ReflexStats += "\nrenderDeltaUs: " + std::to_string(renderDeltaUs);
                m_context->activeSettings().ReflexStats += "\npresentDeltaUs: " + std::to_string(presentDeltaUs);
                m_context->activeSettings().ReflexStats += "\ndriverDeltaUs: " + std::to_string(driverDeltaUs);
                m_context->activeSettings().ReflexStats += "\nosRenderQueueDeltaUs: " + std::to_string(osRenderQueueDeltaUs);
                m_context->activeSettings().ReflexStats += "\ngpuRenderDeltaUs: " + std::to_string(gpuRenderDeltaUs);
            }
            else
            {
                m_context->activeSettings().ReflexStats = "Latency Report Unavailable";
            }
        }
    }

    // DLSS-G Setup
    {
        const auto actualDLSSFGMode = m_context->activeSettings().actualDLSSFGMode();
        const bool wasDLSSFGEnabled = m_context->activeSettings().DLSSFGOptions.mode == StreamlineInterface::DLSSGMode::eOn;

        // If DLSS-G has been turned off, then we tell tell SL to clean it up expressly
        if (wasDLSSFGEnabled && actualDLSSFGMode == StreamlineInterface::DLSSGMode::eOff) {
            streamline.cleanupDLSSG(true);
        }

        // This is where DLSS-G is toggled On and Off (using dlssgOptions.mode) and where we set DLSS-G parameters.
        auto dlssgOptions = StreamlineInterface::DLSSGOptions{};
        dlssgOptions.mode = actualDLSSFGMode;
        dlssgOptions.numFramesToGenerate = m_context->activeSettings().DLSSFGNumFramesToGenerate;

        // This is where we query DLSS-G minimum swapchain size
        if (m_context->activeSettings().IsDLSSFGSupported &&
            (actualDLSSFGMode != StreamlineInterface::DLSSGMode::eOff || wasDLSSFGEnabled))
        {
            StreamlineInterface::DLSSGState state;
            streamline.getDLSSGState(state, dlssgOptions);
            m_context->activeSettings().DLSSFGMultiplier = state.numFramesActuallyPresented;
            m_context->activeSettings().DLSSFGMaxNumFramesToGenerate = state.numFramesToGenerateMax;

            streamline.setDLSSGOptions(dlssgOptions);
            m_context->activeSettings().DLSSFGOptions = dlssgOptions;
        }
        else
        {
            m_context->activeSettings().DLSSFGMultiplier = 1;
            m_context->activeSettings().DLSSFGOptions = dlssgOptions;
        }
    }

    // Ensure DLSS / DLSS-RR is available
    if (m_context->activeSettings().RealtimeAA == 3 && !m_context->activeSettings().IsDLSSRRSupported)
    {
        caustica::warning("Requested DLSS-RR mode not available. Switching to DLSS. ");
        m_context->activeSettings().RealtimeAA = 2;
    }
    if ( m_context->activeSettings().RealtimeAA == 2 && !m_context->activeSettings().IsDLSSSuported )
    {
        caustica::warning("Requested DLSS mode not available. Switching to TAA. ");
        m_context->activeSettings().RealtimeAA = 1;
    }

    // Setup DLSS
    const bool changeToDLSSMode = (m_context->activeSettings().RealtimeAA >= 2 && m_context->activeSettings().RealtimeAA <= 3) && m_context->activeSettings().DLSSLastRealtimeAA != m_context->activeSettings().RealtimeAA;
    {
        // reset DLSS vars if we stop using it
        if (changeToDLSSMode || m_context->activeSettings().DLSSMode == StreamlineInterface::DLSSMode::eOff)
        {
            m_context->activeSettings().DLSSLastMode = PathTracerSettings::DLSSModeDefault;
            m_context->activeSettings().DLSSMode = PathTracerSettings::DLSSModeDefault;
            m_context->activeSettings().DLSSLastDisplaySize = { 0,0 };
        }

        m_context->activeSettings().DLSSLastRealtimeAA = m_context->activeSettings().RealtimeAA;

        // If we are using DLSS set its constants
        if ((m_context->activeSettings().RealtimeAA == 2 || m_context->activeSettings().RealtimeAA == 3) && m_context->activeSettings().RealtimeMode)
        {
            StreamlineInterface::DLSSOptions dlssOptions = {};
            if (m_context->activeSettings().IsDLSSSuported)
            {
                dlssOptions.mode = m_context->activeSettings().DLSSMode;
                dlssOptions.outputWidth = m_displaySize.x;
                dlssOptions.outputHeight = m_displaySize.y;
                dlssOptions.sharpness = 0; //m_recommendedDLSSSettings.sharpness;    // <- is this no longer valid?
                dlssOptions.colorBuffersHDR = true;
                dlssOptions.useAutoExposure = true;     // Optional: provide proper "kBufferTypeExposure" for 0-lag for better precision handling
                dlssOptions.preset = StreamlineInterface::DLSSPreset::eDefault;
                // if (m_context->activeSettings().RealtimeAA < 4) <- docs https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md#50-provide-dlss--dlss-rr-options seem to imply that these should be set even when DLSS-RR enabled
                    streamline.setDLSSOptions(dlssOptions);
            }
            else
            {
                assert( false ); // shouldn't happen, code above should have dropped us to "m_context->activeSettings().RealtimeAA = 1" - check for recent code changes.
            }

            if (m_context->activeSettings().RealtimeAA == 2 || m_context->activeSettings().RealtimeAA == 3)
            {
                // Check if we need to update the rendertarget size.
                bool dlssResizeRequired = (m_context->activeSettings().DLSSMode != m_context->activeSettings().DLSSLastMode) || (m_displaySize.x != m_context->activeSettings().DLSSLastDisplaySize.x) || (m_displaySize.y != m_context->activeSettings().DLSSLastDisplaySize.y);
                if (dlssResizeRequired)
                {
                    // Only quality, target width and height matter here
                    streamline.queryDLSSOptimalSettings(dlssOptions, m_recommendedDLSSSettings);

                    // this is an example on how to override defaults - overriding default 2/3 to higher res 3/4
                    if (dlssOptions.mode == SI::DLSSMode::eMaxQuality)
                    {
                        m_recommendedDLSSSettings.optimalRenderSize.x = dm::clamp((int)(dlssOptions.outputWidth * 3 / 4 + 0.5f), m_recommendedDLSSSettings.minRenderSize.x, m_recommendedDLSSSettings.maxRenderSize.x);
                        m_recommendedDLSSSettings.optimalRenderSize.y = dm::clamp((int)(dlssOptions.outputHeight * 3 / 4 + 0.5f), m_recommendedDLSSSettings.minRenderSize.y, m_recommendedDLSSSettings.maxRenderSize.y);
                    }

                    if (m_recommendedDLSSSettings.optimalRenderSize.x <= 0 || m_recommendedDLSSSettings.optimalRenderSize.y <= 0)
                    {
                        m_context->activeSettings().RealtimeAA = 0;
                        m_context->activeSettings().DLSSMode = PathTracerSettings::DLSSModeDefault;
                        m_renderSize = m_displaySize;
                    }
                    else
                    {
                        m_context->activeSettings().DLSSLastMode = m_context->activeSettings().DLSSMode;
                        m_context->activeSettings().DLSSLastDisplaySize = m_displaySize;
                    }
                }

                m_renderSize = (uint2)m_recommendedDLSSSettings.optimalRenderSize;
            }

            if (m_context->activeSettings().RealtimeAA == 3) // DLSS-RR
            {
                StreamlineInterface::DLSSRROptions dlssRROptions = {};
                dlssRROptions.mode              	= dlssOptions.mode;
                dlssRROptions.outputWidth       	= dlssOptions.outputWidth;
                dlssRROptions.outputHeight      	= dlssOptions.outputHeight;
                dlssRROptions.sharpness         	= dlssOptions.sharpness;
                dlssRROptions.preExposure       	= dlssOptions.preExposure;
                dlssRROptions.exposureScale     	= dlssOptions.exposureScale;
                dlssRROptions.colorBuffersHDR   	= dlssOptions.colorBuffersHDR;
                dlssRROptions.indicatorInvertAxisX 	= dlssOptions.indicatorInvertAxisX;
                dlssRROptions.indicatorInvertAxisY 	= dlssOptions.indicatorInvertAxisY;
                dlssRROptions.normalRoughnessMode 	= StreamlineInterface::DLSSRRNormalRoughnessMode::ePacked;
                dlssRROptions.alphaUpscalingEnabled = false;
                dlssRROptions.preset                = m_context->activeSettings().DLSRRPreset;
                m_lastDLSSRROptions = dlssRROptions; // we need to fill them up with view info, but we can only have proper view after it was initialized with correct RT size
            }
        }
        else
        {
            if (m_context->activeSettings().IsDLSSSuported)
            {
                StreamlineInterface::DLSSOptions dlssOptions = {};
                dlssOptions.mode = StreamlineInterface::DLSSMode::eOff;
                streamline.setDLSSOptions(dlssOptions);
            }

            m_renderSize = m_displaySize;
        }
    }
#else
    const bool changeToDLSSMode = false;
#endif // #if CAUSTICA_WITH_STREAMLINE
}
#endif

#if CAUSTICA_WITH_NATIVE_DLSS
void caustica::render::WorldRenderer::nativeDLSSPreRender()
{
    if (!m_context->activeSettings().RealtimeMode)
    {
        m_renderSize = m_displaySize;
        return;
    }

    if (m_nativeDLSS)
    {
        m_context->activeSettings().IsDLSSSuported = m_nativeDLSS->isDlssSupported();
        m_context->activeSettings().IsDLSSRRSupported = m_nativeDLSS->isRayReconstructionSupported();
    }

    if (m_context->activeSettings().RealtimeAA == 3 && !m_context->activeSettings().IsDLSSRRSupported)
    {
        caustica::warning("Requested DLSS-RR mode not available. Switching to DLSS.");
        m_context->activeSettings().RealtimeAA = 2;
    }

    if (m_context->activeSettings().RealtimeAA == 2 && !m_context->activeSettings().IsDLSSSuported)
    {
        caustica::warning("Requested DLSS mode not available. Switching to TAA.");
        m_context->activeSettings().RealtimeAA = 1;
    }

    const bool usingDLSS = (m_context->activeSettings().RealtimeAA == 2 || m_context->activeSettings().RealtimeAA == 3);
    const bool changeToDLSSMode = usingDLSS && m_context->activeSettings().DLSSLastRealtimeAA != m_context->activeSettings().RealtimeAA;

    if (changeToDLSSMode || m_context->activeSettings().DLSSMode == SI::DLSSMode::eOff)
    {
        m_context->activeSettings().DLSSLastMode = PathTracerSettings::DLSSModeDefault;
        m_context->activeSettings().DLSSMode = PathTracerSettings::DLSSModeDefault;
        m_context->activeSettings().DLSSLastDisplaySize = { 0, 0 };
    }

    m_context->activeSettings().DLSSLastRealtimeAA = m_context->activeSettings().RealtimeAA;

    if (usingDLSS)
    {
        const bool dlssResizeRequired =
            (m_context->activeSettings().DLSSMode != m_context->activeSettings().DLSSLastMode) ||
            (m_displaySize.x != m_context->activeSettings().DLSSLastDisplaySize.x) ||
            (m_displaySize.y != m_context->activeSettings().DLSSLastDisplaySize.y);

        if (dlssResizeRequired)
        {
            m_context->activeSettings().DLSSLastMode = m_context->activeSettings().DLSSMode;
            m_context->activeSettings().DLSSLastDisplaySize = m_displaySize;
        }

        m_renderSize = GetNativeDLSSRenderSize(m_displaySize, m_context->activeSettings().DLSSMode);
    }
    else
    {
        m_renderSize = m_displaySize;
    }
}
#endif

#if CAUSTICA_WITH_NATIVE_DLSS
bool caustica::render::WorldRenderer::evaluateNativeDLSS(bool reset)
{
    if (!m_nativeDLSS || !(m_context->activeSettings().RealtimeAA == 2 || m_context->activeSettings().RealtimeAA == 3))
        return false;

    const bool useRayReconstruction = m_context->activeSettings().RealtimeAA == 3;
    if (useRayReconstruction && !m_nativeDLSS->isRayReconstructionSupported())
        return false;
    if (!useRayReconstruction && !m_nativeDLSS->isDlssSupported())
        return false;

    if (useRayReconstruction)
    {
        RAII_SCOPE(m_commandList->beginMarker("DLSSRR_PrepareInputs");, m_commandList->endMarker(););

        SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        nvrhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();
        m_postProcess->apply(m_commandList, PostProcess::ComputePassType::DLSSRRDenoiserPrepareInputs,
            m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
    }

    caustica::render::DLSS::InitParameters initParams;
    initParams.inputWidth = m_renderSize.x;
    initParams.inputHeight = m_renderSize.y;
    initParams.outputWidth = m_displaySize.x;
    initParams.outputHeight = m_displaySize.y;
    initParams.useLinearDepth = false;
    initParams.useAutoExposure = true;
    initParams.useRayReconstruction = useRayReconstruction;

    m_nativeDLSS->init(initParams);

    const bool initialized = useRayReconstruction
        ? m_nativeDLSS->isRayReconstructionInitialized()
        : m_nativeDLSS->isDlssInitialized();
    if (!initialized)
        return false;

    caustica::render::DLSS::EvaluateParameters evaluateParams;
    evaluateParams.inputColorTexture = m_renderTargets->outputColor;
    evaluateParams.outputColorTexture = m_renderTargets->processedOutputColor;
    evaluateParams.depthTexture = m_renderTargets->depth;
    evaluateParams.motionVectorsTexture = m_renderTargets->screenMotionVectors;
    evaluateParams.motionVectorScaleX = 1.0f / float(m_renderSize.x);
    evaluateParams.motionVectorScaleY = 1.0f / float(m_renderSize.y);
    evaluateParams.resetHistory = reset || m_context->activeSettings().ResetRealtimeCaches;

    if (useRayReconstruction)
    {
        evaluateParams.diffuseAlbedo = m_renderTargets->rrDiffuseAlbedo;
        evaluateParams.specularAlbedo = m_renderTargets->rrSpecAlbedo;
        evaluateParams.normalRoughness = m_renderTargets->rrNormalsAndRoughness;
    }

    const bool evaluated = m_nativeDLSS->evaluate(m_commandList, evaluateParams, *m_context->camera.view());
    if (evaluated)
    {
        static bool loggedNativeDLSSEvaluation = false;
        if (!loggedNativeDLSSEvaluation)
        {
            caustica::info("Native NGX %s evaluated successfully at %ux%u -> %ux%u.",
                useRayReconstruction ? "DLSS-RR" : "DLSS",
                m_renderSize.x, m_renderSize.y, m_displaySize.x, m_displaySize.y);
            loggedNativeDLSSEvaluation = true;
        }
    }

    return evaluated;
}
#endif

void caustica::render::WorldRenderer::runDlssUpscale(nvrhi::ICommandList* commandList, bool reset)
{
    if (!m_context->activeSettings().RealtimeMode)
        return;

    if (!(m_context->activeSettings().RealtimeAA == 2 || m_context->activeSettings().RealtimeAA == 3))
        return;

    PostProcessAAParams params{
        m_context->activeSettings(),
        commandList,
        m_renderTargets.get(),
        &m_context->gpuDevice,
    };
    params.renderSize = m_renderSize;
    params.displaySize = m_displaySize;
    params.displayAspectRatio = m_displayAspectRatio;
    params.cameraJitter = computeCameraJitter();
    params.sampleIndex = m_sampleIndex;
    params.frameIndex = static_cast<uint32_t>(m_frameIndex);
    params.reset = reset;
    params.temporalAAPass = m_temporalAntiAliasingPass.get();
    params.accumulationPass = m_accumulationPass.get();
    params.postProcess = m_postProcess.get();
    params.bindingSet = m_bindingSet;
    params.bindingLayout = m_bindingLayout;
    params.constantBuffer = m_constantBuffer;
    params.accumulationSampleIndex = m_accumulationSampleIndex;
    params.gaussianSplatTemporalSampleIndex = &m_gaussianSplatTemporalSampleIndex;
    params.gaussianSplatTemporalReset = &m_frameGaussianSplatTemporalReset;
#if CAUSTICA_WITH_STREAMLINE
    params.dlssRROptions = &m_lastDLSSRROptions;
#endif

    caustica::postProcessAA(m_context->camera, params);

#if CAUSTICA_WITH_NATIVE_DLSS
    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    bool nativeDLSSEvaluated = evaluateNativeDLSS(reset);

    if (!nativeDLSSEvaluated)
    {
        if (m_context->activeSettings().actualUseStandaloneDenoiser())
        {
            commandList->copyTexture(
                m_renderTargets->processedOutputColor, nvrhi::TextureSlice(),
                m_renderTargets->outputColor, nvrhi::TextureSlice());
        }
        else
        {
            SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
            nvrhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();
            commandList->beginMarker("NoDenoiserFinalMerge");
            m_postProcess->apply(
                commandList,
                PostProcess::ComputePassType::NoDenoiserFinalMerge,
                m_constantBuffer,
                miniConstants,
                m_bindingSet,
                m_bindingLayout,
                tdesc.width,
                tdesc.height);
            commandList->endMarker();
        }
    }

    m_commandList = savedCommandList;
#endif
}

void caustica::render::WorldRenderer::postProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset)
{
    (void)framebuffer;
    runDlssUpscale(m_commandList, reset);
}


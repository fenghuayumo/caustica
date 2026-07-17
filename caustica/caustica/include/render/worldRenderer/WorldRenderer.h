#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>

#include <render/core/CameraController.h>
#include <render/worldRenderer/PathTracingContext.h>
#include <shaders/PathTracer/Config.h>
#include <shaders/SampleConstantBuffer.h>
#include <render/core/RenderTargets.h>
#include <render/passes/postProcess/PostProcess.h>
#include <render/passes/postProcess/AccumulationPass.h>
#include <render/passes/geometry/BloomPass.h>
#include <render/passes/denoisers/NrdIntegration.h>
#include <render/passes/rtxdi/RtxdiPass.h>
#include <render/passes/debug/ShaderDebug.h>
#include <render/passes/gaussian/GaussianSplatEmissionProxy.h>

#include <render/ecs/RenderFrameContext.h>
#include <render/pipeline/RenderPipelineRegistry.h>
#include <render/graph/RenderTargetPool.h>
#include <render/graph/RenderBufferPool.h>
#include <render/worldRenderer/PathTracingFrameContext.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#if CAUSTICA_WITH_STREAMLINE
#include <backend/StreamlineInterface.h>
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
#include <render/passes/geometry/DLSS.h>
#endif

class ComputePipelineRegistry;
class OidnDenoiser;
class DenoisingGuidesPass;
class PathTracingShaderCompiler;
class PTPipelineVariant;
class ToneMappingPass;
struct PathTracerCameraData;
struct PathTracerConstants;

namespace caustica
{
class ICompositeView;
class IView;
namespace render
{
class TemporalAntiAliasingPass;
class BloomPass;
class DLSS;
class GPUSort;
class RenderGraphRegistry;
class PathTracingPipelinePlugin;
struct ExtractedFrameView;
struct RenderFeatureContext;

// =============================================================================
// WorldRenderer — GPU path-tracing pipeline driven by PathTracingContext.
// =============================================================================
class WorldRenderer
{
public:
    WorldRenderer(PathTracingContext& context);
    ~WorldRenderer();

    static nvrhi::BindingLayoutHandle createBindlessLayout(nvrhi::IDevice* device);
    void createBindingLayouts(nvrhi::IBindingLayout* precreatedBindless = nullptr);
    void createDeviceResources();
    void onBackBufferResizing();
    void preRender();
    void render(nvrhi::IFramebuffer* framebuffer);

    void pathTracePrePass(nvrhi::ICommandList* commandList);
    void vBufferExport(nvrhi::ICommandList* commandList);
    void pathTraceLightingEndUpdate(nvrhi::ICommandList* commandList);
    void mainPathTrace(nvrhi::ICommandList* commandList);
    void executeRtxdi(nvrhi::ICommandList* commandList);
    void prepareDenoiserGuides(nvrhi::ICommandList* commandList);
    void stablePlanesDebugViz(nvrhi::ICommandList* commandList);
    void ensureNrdIntegrations();
    void denoiseStablePlane(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, int planeIndex);
    void denoise(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer);
    void runDlssUpscale(nvrhi::ICommandList* commandList, bool reset);
    void runNoDenoiserFinalMerge(nvrhi::ICommandList* commandList);
    [[nodiscard]] bool hasActiveGaussianSplats() const;
    void prepareGaussianSplatPasses();
    void buildGaussianSplatEmissionProxies();
    void executeGaussianSplatAccelBuild(nvrhi::ICommandList* commandList);
    void executeGaussianSplatRender(nvrhi::ICommandList* commandList, bool renderToOutputColor);
    void executeGaussianSplatAccumulate(nvrhi::ICommandList* commandList);
    [[nodiscard]] const std::vector<GaussianSplatEmissionProxy>& gaussianSplatEmissionProxies() const
    {
        return m_gaussianSplatEmissionProxies;
    }
    void postProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset);
    void recreateBindingSet();
    void onSceneUnloading();
    void onSceneLoaded();
    void invalidateBindingSet() { m_bindingSet = nullptr; }
    void resetFrameIndex();

    bool createPTPipeline();

#if CAUSTICA_WITH_STREAMLINE
    void streamlinePreRender();
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
    void nativeDLSSPreRender();
#endif

    RenderTargets* getRenderTargets() { return m_renderTargets.get(); }
    const RenderTargets* getRenderTargets() const { return m_renderTargets.get(); }

    nvrhi::BindingLayoutHandle getBindingLayout() const { return m_bindingLayout; }
    nvrhi::BindingLayoutHandle getBindlessLayout() const { return m_bindlessLayout; }
    nvrhi::BindingSetHandle getBindingSet() const { return m_bindingSet; }

    RtxdiPass* getRtxdiPass() { return m_rtxdiPass.get(); }
    const RtxdiPass* getRtxdiPass() const { return m_rtxdiPass.get(); }

    std::shared_ptr<PathTracingShaderCompiler> getPathTracingShaderCompiler() const { return m_pathTracingShaderCompiler; }
    std::shared_ptr<ShaderDebug> getShaderDebug() const { return m_shaderDebug; }

    std::shared_ptr<PTPipelineVariant>& ptPipelineReference() { return m_ptPipelineReference; }
    std::shared_ptr<PTPipelineVariant>& ptPipelineBuildStablePlanes() { return m_ptPipelineBuildStablePlanes; }
    std::shared_ptr<PTPipelineVariant>& ptPipelineFillStablePlanes() { return m_ptPipelineFillStablePlanes; }
    std::shared_ptr<PTPipelineVariant>& ptPipelineTestRaygenPPHDR() { return m_ptPipelineTestRaygenPPHDR; }
    std::shared_ptr<PTPipelineVariant>& ptPipelineEdgeDetection() { return m_ptPipelineEdgeDetection; }

    nvrhi::CommandListHandle getCommandList() const { return m_commandList; }
    nvrhi::BufferHandle getConstantBuffer() const { return m_constantBuffer; }

    TemporalAntiAliasingPass* getTemporalAntiAliasingPass() { return m_temporalAntiAliasingPass.get(); }
    AccumulationPass* getAccumulationPass() { return m_accumulationPass.get(); }
    BloomPass* getBloomPass() { return m_bloomPass.get(); }
    ToneMappingPass* getToneMappingPass() { return m_toneMappingPass.get(); }
    CameraController& getCameraController() { return m_context.camera; }
    PathTracingContext& getPathTracingContext() { return m_context; }
    const PathTracingContext& getPathTracingContext() const { return m_context; }

    dm::uint2 getRenderSize() const { return m_renderSize; }
    dm::uint2 getDisplaySize() const { return m_displaySize; }
    float getDisplayAspectRatio() const { return m_displayAspectRatio; }

    uint64_t getFrameIndex() const { return m_frameIndex; }
    uint getSampleIndex() const { return m_sampleIndex; }
    int getAccumulationSampleIndex() const { return m_accumulationSampleIndex; }
    bool getAccumulationCompleted() const { return m_accumulationCompleted; }

    const DebugFeedbackStruct& getFeedbackData() const { return m_feedbackData; }
    // Picking flags from the frame snapshot that just finished rendering (not live UI state).
    [[nodiscard]] const RenderPickState& getLastRenderedPicking() const { return m_lastRenderedPicking; }
    const DeltaTreeVizPathVertex* getDebugDeltaPathTree() const { return m_debugDeltaPathTree; }

    std::vector<DebugLineStruct>& getCpuSideDebugLines() { return m_cpuSideDebugLines; }

    void setGaussianSplatTemporalReset(bool v) { m_gaussianSplatTemporalReset = v; }
    [[nodiscard]] bool consumeGaussianSplatTemporalReset()
    {
        const bool value = m_gaussianSplatTemporalReset;
        m_gaussianSplatTemporalReset = false;
        return value;
    }

#if CAUSTICA_WITH_NATIVE_DLSS
    DLSS* getNativeDLSS() { return m_nativeDLSS.get(); }
#endif

    void denoisedScreenshot(nvrhi::ITexture* framebufferTexture) const;

    void buildFrameGraphPasses(RenderFrameContext& ctx, const RenderGraphRegistry& graphRegistry);
    void executeFrameRenderGraph(RenderFrameContext& ctx);
    void registerDebugOverlayGraphPasses(RenderFeatureContext ctx);

    void addRenderPipelinePlugin(std::unique_ptr<IRenderPipelinePlugin> plugin);
    void addRenderPipelinePlugin(IRenderPipelinePlugin& plugin);
    [[nodiscard]] RenderPipelineRegistry& pipelineRegistry() { return m_pipelineRegistry; }
    [[nodiscard]] const RenderPipelineRegistry& pipelineRegistry() const { return m_pipelineRegistry; }

private:
    friend class PathTracingPipelinePlugin;
    friend class RenderPipelineRegistry;

    [[nodiscard]] nvrhi::IDevice* device() const { return m_context.gpuDevice.getDevice(); }

    [[nodiscard]] CameraUpdateParams makeCameraUpdateParams() const;
    void syncCameraViews();
    [[nodiscard]] dm::float2 computeCameraJitter() const;

    void populateRenderFrameContext(nvrhi::IFramebuffer* framebuffer, RenderFrameContext& ctx);
    void populateFrameView(ExtractedFrameView& view);
    [[nodiscard]] RenderFeatureContext makeRenderFeatureContext(RenderFrameContext& ctx);
    void framePassSetup(PathTracingFrameContext& ctx);
    void framePassEnsureRenderTargets(PathTracingFrameContext& ctx);
    void framePassRendererInit(PathTracingFrameContext& ctx);
    void framePassShaderUpdate(PathTracingFrameContext& ctx);
    void framePassBeginCommandList(PathTracingFrameContext& ctx);
    void framePassSceneUpdate(PathTracingFrameContext& ctx);
    void framePassPathTracePrepare(PathTracingFrameContext& ctx);
    void framePassPathTrace(PathTracingFrameContext& ctx);
    void framePassDenoiseAndAA(PathTracingFrameContext& ctx);
    void framePassFinalize(PathTracingFrameContext& ctx);

    void createRenderPasses(bool& exposureResetRequired, nvrhi::CommandListHandle initializeCommandList);
    void preUpdateLighting(nvrhi::CommandListHandle commandList, bool& needNewBindings);
    void updateLighting(nvrhi::CommandListHandle commandList);
    void preUpdatePathTracing(bool resetAccum, nvrhi::CommandListHandle commandList);
    void postUpdatePathTracing();
    void updatePathTracerConstants(PathTracerConstants& constants, const PathTracerCameraData& cameraData);
    void rtxdiSetupFrame(nvrhi::IFramebuffer* framebuffer, PathTracerCameraData cameraData, dm::uint2 renderDims);

    void resetReferenceOIDN();
    void applyReferenceOIDN();
#if CAUSTICA_WITH_NATIVE_DLSS
    bool evaluateNativeDLSS(bool reset);
#endif

    PathTracingContext&          m_context;

    RenderPipelineRegistry       m_pipelineRegistry;
    rg::GraphBuilder             m_frameGraph;
    rg::RenderTargetPool         m_renderTargetPool;
    rg::RenderBufferPool         m_renderBufferPool;
    RenderFrameContext           m_renderFrameCtx{};

    std::unique_ptr<RtxdiPass>                  m_rtxdiPass;
    std::unique_ptr<RenderTargets>              m_renderTargets;
    nvrhi::BindingLayoutHandle                  m_bindingLayout;
    nvrhi::BindingLayoutHandle                  m_bindlessLayout;
    nvrhi::BindingSetHandle                     m_bindingSet;

    std::shared_ptr<PTPipelineVariant>          m_ptPipelineReference;
    std::shared_ptr<PTPipelineVariant>          m_ptPipelineBuildStablePlanes;
    std::shared_ptr<PTPipelineVariant>          m_ptPipelineFillStablePlanes;
    std::shared_ptr<PTPipelineVariant>          m_ptPipelineTestRaygenPPHDR;
    std::shared_ptr<PTPipelineVariant>          m_ptPipelineEdgeDetection;
    std::shared_ptr<PathTracingShaderCompiler>            m_pathTracingShaderCompiler;

    nvrhi::CommandListHandle                    m_commandList;
    nvrhi::BufferHandle                         m_constantBuffer;

    std::unique_ptr<TemporalAntiAliasingPass>    m_temporalAntiAliasingPass;
    std::unique_ptr<BloomPass>                  m_bloomPass;
    std::unique_ptr<ToneMappingPass>            m_toneMappingPass;
    std::shared_ptr<PostProcess>                m_postProcess;

    std::unique_ptr<NrdIntegration>             m_nrd[cStablePlaneCount];
    std::unique_ptr<AccumulationPass>           m_accumulationPass;
    std::unique_ptr<OidnDenoiser>               m_oidnDenoiser;
    nvrhi::TextureHandle                        m_oidnDenoisedOutput;
    bool                                        m_oidnDenoisedOutputValid = false;
    bool                                        m_oidnDenoiserFailed = false;

    std::shared_ptr<ShaderDebug>                m_shaderDebug;
    std::shared_ptr<DenoisingGuidesPass>       m_denoisingGuidesPass;

    nvrhi::ShaderHandle                         m_exportVBufferCS;
    nvrhi::ComputePipelineHandle                m_exportVBufferPSO;

#if CAUSTICA_WITH_STREAMLINE
    caustica::StreamlineInterface::DLSSSettings   m_recommendedDLSSSettings = {};
    caustica::StreamlineInterface::DLSSRROptions  m_lastDLSSRROptions;
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
    std::unique_ptr<DLSS>                       m_nativeDLSS;
#endif

    dm::uint2                                   m_renderSize{};
    dm::uint2                                   m_displaySize{};
    float                                       m_displayAspectRatio = 1.0f;

    int                                         m_accumulationSampleIndex = 0;
    uint64_t                                    m_frameIndex = 0;
    uint                                        m_sampleIndex = 0;
    SampleConstants                             m_currentConstants = {};
    bool                                        m_accumulationCompleted = false;
    bool                                        m_lastRealtimeMode = true;
    int                                         m_lastScheduledRealtimeAA = -1;

    nvrhi::TextureHandle                        m_gaussianSplatCurrentColor;
    nvrhi::TextureHandle                        m_gaussianSplatAccumulatedColor;
    std::unique_ptr<AccumulationPass>           m_gaussianSplatAccumulationPass;
    std::shared_ptr<GPUSort>                    m_gaussianSplatGpuSort;
    std::vector<GaussianSplatEmissionProxy>     m_gaussianSplatEmissionProxies;
    int                                         m_gaussianSplatTemporalSampleIndex = 0;
    bool                                        m_gaussianSplatTemporalReset = true;
    bool                                        m_gaussianSplatCompositeRendered = false;

    // Per-frame copies from SceneRenderData (filled at render() begin).
    PathTracerSettings                          m_frameSettingsSnapshot;
    RenderRuntimeState                          m_frameRuntimeSnapshot;
    RenderPickState                             m_lastRenderedPicking{};
    bool                                        m_frameGaussianSplatTemporalReset = false;

    nvrhi::BufferHandle                         m_feedback_Buffer_Gpu;
    nvrhi::BufferHandle                         m_feedback_Buffer_Cpu;
    nvrhi::BufferHandle                         m_debugLineBufferCapture;
    nvrhi::BufferHandle                         m_debugLineBufferDisplay;
    nvrhi::ShaderHandle                         m_linesVertexShader;
    nvrhi::ShaderHandle                         m_linesPixelShader;
    std::vector<DebugLineStruct>                m_cpuSideDebugLines;
    nvrhi::InputLayoutHandle                    m_linesInputLayout;
    nvrhi::GraphicsPipelineHandle               m_linesPipeline;
    nvrhi::BindingLayoutHandle                  m_linesBindingLayout;
    nvrhi::BindingSetHandle                     m_linesBindingSet;

    DebugFeedbackStruct                         m_feedbackData{};
    DeltaTreeVizPathVertex                      m_debugDeltaPathTree[cDeltaTreeVizMaxVertices]{};
    nvrhi::BufferHandle                         m_debugDeltaPathTree_Gpu;
    nvrhi::BufferHandle                         m_debugDeltaPathTree_Cpu;
    nvrhi::BufferHandle                         m_debugDeltaPathTreeSearchStack;
};

} // namespace render
} // namespace caustica

#pragma once

#include <math/math.h>
#include <rhi/rhi.h>

#include <render/AppDiagnostics.h>
#include <render/RenderRuntimeState.h>
#include <render/core/AccelStructManager.h>
#include <render/core/CameraController.h>
#include <render/core/PathTracerSettings.h>
#include <render/PathTracerScenePasses.h>
#include <render/PathTracingContext.h>
#include <shaders/PathTracer/Config.h>
#include <shaders/SampleConstantBuffer.h>
#include <render/core/RenderTargets.h>
#include <render/passes/postProcess/PostProcess.h>
#include <render/passes/postProcess/AccumulationPass.h>
#include <render/passes/geometry/BloomPass.h>
#include <render/passes/denoisers/DenoisePass.h>
#include <render/passes/rtxdi/RtxdiPass.h>
#include <render/passes/pathTrace/PathTracePass.h>
#include <render/passes/debug/ShaderDebug.h>
#include <render/passes/gaussian/GaussianSplatEmissionProxy.h>
#include <render/passes/gaussian/GaussianSplatFramePass.h>

#include <render/ecs/RenderFrameContext.h>
#include <render/pipeline/RenderPipelineRegistry.h>
#include <render/graph/RenderTargetPool.h>
#include <render/graph/RenderBufferPool.h>
#include <render/PathTracingFrameContext.h>

#include <chrono>
#include <filesystem>
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
class PathTracingShaderCompiler;
class PTPipelineVariant;
class ToneMappingPass;
struct PathTracerCameraData;

namespace caustica::render { class RtPipelineCache; }
struct PathTracerConstants;

namespace caustica
{
class GpuDevice;
class ICompositeView;
class IView;
class Scene;
struct GpuSharedCaches;
namespace scene { class SceneRenderData; }

namespace render
{
class TemporalAntiAliasingPass;
class BloomPass;
class DLSS;
class RenderGraphRegistry;
class PathTracingPipelinePlugin;
struct ExtractedFrameView;
struct FrameGraphContext;

// =============================================================================
// WorldRenderer — GPU path-tracing pipeline and runtime ownership.
// =============================================================================
class WorldRenderer
{
public:
    WorldRenderer();
    ~WorldRenderer();

    WorldRenderer(const WorldRenderer&) = delete;
    WorldRenderer& operator=(const WorldRenderer&) = delete;
    WorldRenderer(WorldRenderer&&) = delete;
    WorldRenderer& operator=(WorldRenderer&&) = delete;

    struct createParams
    {
        GpuDevice& gpuDevice;
        GpuSharedCaches& gpuSharedCaches;
        ::PathTracerSettings& settings;
        RenderRuntimeState& runtimeState;
        AppDiagnostics& diagnostics;
        double& sceneTime;
    };

    bool create(const createParams& params);
    void destroy();

    [[nodiscard]] CameraController& renderCamera() { return m_renderCamera; }
    [[nodiscard]] const CameraController& renderCamera() const { return m_renderCamera; }
    [[nodiscard]] AccelStructManager& accelStructs() { return m_accelStructs; }
    [[nodiscard]] const AccelStructManager& accelStructs() const { return m_accelStructs; }
    [[nodiscard]] SceneLightingPasses& lightingPasses() { return m_scenePasses.lighting; }
    [[nodiscard]] const SceneLightingPasses& lightingPasses() const { return m_scenePasses.lighting; }
    [[nodiscard]] SceneRayTracingResources& rayTracingResources() { return m_scenePasses.rayTracing; }
    [[nodiscard]] const SceneRayTracingResources& rayTracingResources() const { return m_scenePasses.rayTracing; }
    [[nodiscard]] SceneGaussianSplatPasses& gaussianSplatPasses() { return m_scenePasses.gaussianSplats; }
    [[nodiscard]] const SceneGaussianSplatPasses& gaussianSplatPasses() const { return m_scenePasses.gaussianSplats; }
    [[nodiscard]] SceneGpuResources& sceneGpuResources() { return m_context->sceneGpuResources; }
    [[nodiscard]] const SceneGpuResources& sceneGpuResources() const { return m_context->sceneGpuResources; }

    static caustica::rhi::BindingLayoutHandle createBindlessLayout(caustica::rhi::IDevice* device);
    void createBindingLayouts(caustica::rhi::IBindingLayout* precreatedBindless = nullptr);
    void createDeviceResources();
    void onBackBufferResizing();
    void preRender();
    void render(caustica::rhi::IFramebuffer* framebuffer);

    void prepareGaussianSplatPasses();
    void buildGaussianSplatEmissionProxies();
    [[nodiscard]] const std::vector<GaussianSplatEmissionProxy>& gaussianSplatEmissionProxies() const
    {
        return m_gaussianSplatEmissionProxies;
    }
    [[nodiscard]] DenoisePass* getDenoisePass() { return m_denoisePass.get(); }
    [[nodiscard]] const DenoisePass* getDenoisePass() const { return m_denoisePass.get(); }
    [[nodiscard]] GaussianSplatFramePass* getGaussianSplatFramePass() { return m_gaussianFramePass.get(); }
    [[nodiscard]] const GaussianSplatFramePass* getGaussianSplatFramePass() const { return m_gaussianFramePass.get(); }
    void recreateBindingSet(const scene::SceneRenderData* renderData = nullptr);
    void onSceneUnloading();
    void onSceneLoaded(std::shared_ptr<Scene> scene, std::filesystem::path scenePath);
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

    caustica::rhi::BindingLayoutHandle getBindingLayout() const { return m_bindingLayout; }
    caustica::rhi::BindingLayoutHandle getBindlessLayout() const { return m_bindlessLayout; }
    caustica::rhi::BindingSetHandle getBindingSet() const { return m_bindingSet; }

    RtxdiPass* getRtxdiPass() { return m_rtxdiPass.get(); }
    const RtxdiPass* getRtxdiPass() const { return m_rtxdiPass.get(); }
    PathTracePass* getPathTracePass() { return m_pathTracePass.get(); }
    const PathTracePass* getPathTracePass() const { return m_pathTracePass.get(); }

    std::shared_ptr<PathTracingShaderCompiler> getPathTracingShaderCompiler() const { return m_pathTracingShaderCompiler; }
    std::shared_ptr<RtPipelineCache> getRtPipelineCache() const { return m_rtPipelineCache; }

    // Explicit load/cook precache of every cooked feature-preset RT PSO bundle.
    // Call on the render thread after the first PT update has a hit-group set.
    uint32_t precacheAllRtFeaturePresets(bool showProgress = true);
    std::shared_ptr<ShaderDebug> getShaderDebug() const { return m_shaderDebug; }

    std::shared_ptr<PTPipelineVariant>& ptPipelineReference() { return m_ptPipelineReference; }
    std::shared_ptr<PTPipelineVariant>& ptPipelineBuildStablePlanes() { return m_ptPipelineBuildStablePlanes; }
    std::shared_ptr<PTPipelineVariant>& ptPipelineFillStablePlanes() { return m_ptPipelineFillStablePlanes; }
    std::shared_ptr<PTPipelineVariant>& ptPipelineTestRaygenPPHDR() { return m_ptPipelineTestRaygenPPHDR; }
    std::shared_ptr<PTPipelineVariant>& ptPipelineEdgeDetection() { return m_ptPipelineEdgeDetection; }

    caustica::rhi::CommandListHandle getCommandList() const { return m_commandList; }
    caustica::rhi::BufferHandle getConstantBuffer() const { return m_constantBuffer; }

    TemporalAntiAliasingPass* getTemporalAntiAliasingPass() { return m_temporalAntiAliasingPass.get(); }
    AccumulationPass* getAccumulationPass() { return m_accumulationPass.get(); }
    BloomPass* getBloomPass() { return m_bloomPass.get(); }
    ToneMappingPass* getToneMappingPass() { return m_toneMappingPass.get(); }
    CameraController& getCameraController() { return m_context->camera; }
    PathTracingContext& getPathTracingContext() { return *m_context; }
    const PathTracingContext& getPathTracingContext() const { return *m_context; }

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

    void denoisedScreenshot(caustica::rhi::ITexture* framebufferTexture) const;

    void buildFrameGraphPasses(RenderFrameContext& ctx, const RenderGraphRegistry& graphRegistry);
    void executeFrameRenderGraph(RenderFrameContext& ctx);

    void addRenderPipelinePlugin(std::unique_ptr<IRenderPipelinePlugin> plugin);
    void addRenderPipelinePlugin(IRenderPipelinePlugin& plugin);
    [[nodiscard]] RenderPipelineRegistry& pipelineRegistry() { return m_pipelineRegistry; }
    [[nodiscard]] const RenderPipelineRegistry& pipelineRegistry() const { return m_pipelineRegistry; }

private:
    friend class PathTracingPipelinePlugin;
    friend class RenderPipelineRegistry;

    [[nodiscard]] caustica::rhi::IDevice* device() const { return m_context->gpuDevice.getDevice(); }

    [[nodiscard]] CameraUpdateParams makeCameraUpdateParams() const;
    void syncCameraViews();
    [[nodiscard]] dm::float2 computeCameraJitter() const;

    void populateRenderFrameContext(caustica::rhi::IFramebuffer* framebuffer, RenderFrameContext& ctx);
    void populateFrameView(ExtractedFrameView& view);
    [[nodiscard]] FrameGraphContext makeFrameGraphContext(RenderFrameContext& ctx);
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

    void createRenderPasses(bool& exposureResetRequired, caustica::rhi::CommandListHandle initializeCommandList);
    void createPostProcessRenderPasses();
    void preUpdatePathTracing(bool resetAccum, caustica::rhi::CommandListHandle commandList);
    void postUpdatePathTracing();

    PathTracerScenePasses        m_scenePasses;
    CameraController             m_renderCamera;
    AccelStructManager           m_accelStructs;
    std::unique_ptr<PathTracingContext> m_pathTracingContext;
    PathTracingContext*          m_context = nullptr;

    RenderPipelineRegistry       m_pipelineRegistry;
    rg::GraphBuilder             m_frameGraph;
    rg::RenderTargetPool         m_renderTargetPool;
    rg::RenderBufferPool         m_renderBufferPool;
    RenderFrameContext           m_renderFrameCtx{};

    std::unique_ptr<RtxdiPass>                  m_rtxdiPass;
    std::unique_ptr<PathTracePass>              m_pathTracePass;
    std::unique_ptr<DenoisePass>                m_denoisePass;
    std::unique_ptr<GaussianSplatFramePass>     m_gaussianFramePass;
    std::unique_ptr<RenderTargets>              m_renderTargets;
    caustica::rhi::BindingLayoutHandle                  m_bindingLayout;
    caustica::rhi::BindingLayoutHandle                  m_bindlessLayout;
    caustica::rhi::BindingSetHandle                     m_bindingSet;

    std::shared_ptr<PTPipelineVariant>          m_ptPipelineReference;
    std::shared_ptr<PTPipelineVariant>          m_ptPipelineBuildStablePlanes;
    std::shared_ptr<PTPipelineVariant>          m_ptPipelineFillStablePlanes;
    std::shared_ptr<PTPipelineVariant>          m_ptPipelineTestRaygenPPHDR;
    std::shared_ptr<PTPipelineVariant>          m_ptPipelineEdgeDetection;
    std::shared_ptr<PathTracingShaderCompiler>            m_pathTracingShaderCompiler;
    std::shared_ptr<RtPipelineCache>                      m_rtPipelineCache;

    caustica::rhi::CommandListHandle                    m_commandList;
    caustica::rhi::BufferHandle                         m_constantBuffer;

    std::unique_ptr<TemporalAntiAliasingPass>    m_temporalAntiAliasingPass;
    std::unique_ptr<BloomPass>                  m_bloomPass;
    std::unique_ptr<ToneMappingPass>            m_toneMappingPass;
    std::shared_ptr<PostProcess>                m_postProcess;

    std::unique_ptr<AccumulationPass>           m_accumulationPass;

    std::shared_ptr<ShaderDebug>                m_shaderDebug;

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

    std::vector<GaussianSplatEmissionProxy>     m_gaussianSplatEmissionProxies;
    int                                         m_gaussianSplatTemporalSampleIndex = 0;
    bool                                        m_gaussianSplatTemporalReset = true;

    // Per-frame copies from SceneRenderData (filled at render() begin).
    PathTracerSettings                          m_frameSettingsSnapshot;
    RenderRuntimeState                          m_frameRuntimeSnapshot;
    RenderPickState                             m_lastRenderedPicking{};
    bool                                        m_frameGaussianSplatTemporalReset = false;

    caustica::rhi::BufferHandle                         m_feedback_Buffer_Gpu;
    caustica::rhi::BufferHandle                         m_feedback_Buffer_Cpu;
    caustica::rhi::BufferHandle                         m_debugLineBufferCapture;
    caustica::rhi::BufferHandle                         m_debugLineBufferDisplay;
    caustica::rhi::ShaderHandle                         m_linesVertexShader;
    caustica::rhi::ShaderHandle                         m_linesPixelShader;
    std::vector<DebugLineStruct>                m_cpuSideDebugLines;
    caustica::rhi::InputLayoutHandle                    m_linesInputLayout;
    caustica::rhi::GraphicsPipelineHandle               m_linesPipeline;
    caustica::rhi::BindingLayoutHandle                  m_linesBindingLayout;
    caustica::rhi::BindingSetHandle                     m_linesBindingSet;

    DebugFeedbackStruct                         m_feedbackData{};
    DeltaTreeVizPathVertex                      m_debugDeltaPathTree[cDeltaTreeVizMaxVertices]{};
    caustica::rhi::BufferHandle                         m_debugDeltaPathTree_Gpu;
    caustica::rhi::BufferHandle                         m_debugDeltaPathTree_Cpu;
    caustica::rhi::BufferHandle                         m_debugDeltaPathTreeSearchStack;
};

} // namespace render
} // namespace caustica

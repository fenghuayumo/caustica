#pragma once

#include <math/math.h>
#include <rhi/rhi.h>

#include <render/AppDiagnostics.h>
#include <render/RenderRuntimeState.h>
#include <render/core/AccelStructManager.h>
#include <render/core/CameraController.h>
#include <render/core/PathTracerSettings.h>
#include <render/PathTraceSceneBindings.h>
#include <render/PathTracerScenePasses.h>
#include <render/PathTracingContext.h>
#include <shaders/PathTracer/Config.h>
#include <shaders/FrameConstantBuffer.h>
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
#include <render/graph/RenderTargetPool.h>
#include <render/graph/RenderBufferPool.h>
#include <render/PathTracingFrameContext.h>

#include <chrono>
#include <atomic>
#include <array>
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
class ToneMappingPass;
struct PathTracerCameraData;

struct PathTracerConstants;

namespace caustica
{
class GpuDevice;
class ICompositeView;
class IView;
class Scene;
struct GpuSharedCaches;
namespace scene
{
class SceneRenderData;
}

namespace render
{
class TemporalAntiAliasingPass;
class BloomPass;
class DLSS;
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

    static caustica::rhi::BindingLayoutHandle createBindlessLayout(caustica::rhi::Device* device);
    void createBindingLayouts(caustica::rhi::BindingLayout* precreatedBindless = nullptr);
    void createDeviceResources();
    void onBackBufferResizing();
    void preRender();
    void render(caustica::rhi::Framebuffer* framebuffer);

    void prepareGaussianSplatPasses();
    void buildGaussianSplatEmissionProxies();
    void recreateBindingSet(const scene::SceneRenderData* renderData = nullptr);
    void createGraphScratchFallbacks();
    void publishGraphScratchBindings(rg::GraphBuilder& graph);
    // Render-thread only. Caller must waitForIdle() first.
    void releaseStreamlineTemporalResources();
    void onSceneUnloading();
    void onSceneLoaded(std::shared_ptr<Scene> scene, std::filesystem::path scenePath);
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

    [[nodiscard]] bool hasSceneBindingSet() const { return m_sceneBindings.ready(); }

    // Explicit load/cook precache of every cooked feature-preset RT PSO bundle.
    // Call on the render thread after the first PT update has a hit-group set.
    uint32_t precacheAllRtFeaturePresets(bool showProgress = true);

    ToneMappingPass* getToneMappingPass() { return m_toneMappingPass.get(); }

    dm::uint2 getRenderSize() const { return m_renderSize; }
    dm::uint2 getDisplaySize() const { return m_displaySize; }

    uint64_t getFrameIndex() const { return m_frameIndex; }
    int getAccumulationSampleIndex() const { return m_accumulationSampleIndex; }
    bool getAccumulationCompleted() const { return m_accumulationCompleted; }

    const DebugFeedbackStruct& getFeedbackData() const { return m_feedbackData; }
    // Picking flags from the frame snapshot that just finished rendering (not live UI state).
    [[nodiscard]] const RenderPickState& getLastRenderedPicking() const { return m_lastRenderedPicking; }
    // Logic-thread fast path for latency-sensitive editor clicks.
    void submitImmediateMaterialPick(const RenderPickState& picking);
    void submitImmediateInstancePick(const RenderPickState& picking);
    const DeltaTreeVizPathVertex* getDebugDeltaPathTree() const { return m_debugDeltaPathTree; }

    std::vector<DebugLineStruct>& getCpuSideDebugLines() { return m_cpuSideDebugLines; }

    void setGaussianSplatTemporalReset(bool v) { m_gaussianSplatTemporalReset = v; }
    [[nodiscard]] bool consumeGaussianSplatTemporalReset()
    {
        const bool value = m_gaussianSplatTemporalReset;
        m_gaussianSplatTemporalReset = false;
        return value;
    }

    void denoisedScreenshot(caustica::rhi::Texture* framebufferTexture) const;

private:
    [[nodiscard]] caustica::rhi::Device* device() const { return m_context->gpuDevice.getDevice(); }
    [[nodiscard]] FrameGraphContext beginFrameGraph(RenderFrameContext& ctx);
    void runFramePipeline(RenderFrameContext& ctx);
    void executeFrameRenderGraph(RenderFrameContext& ctx);

    [[nodiscard]] CameraUpdateParams makeCameraUpdateParams() const;
    void syncCameraViews();
    [[nodiscard]] dm::float2 computeCameraJitter() const;

    void populateRenderFrameContext(caustica::rhi::Framebuffer* framebuffer, RenderFrameContext& ctx);
    void populateFrameView(ExtractedFrameView& view);
    void mergeImmediateMaterialPick();
    void mergeImmediateInstancePick();
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
    // ADR 0002 S1: map CPU feedback after graphics-queue EventQuery (not device idle).
    void mapDebugFeedbackReadback();
    // ADR 0002 S2: wait last graphics submit (EventQuery), not device-wide idle.
    // Falls back to waitForIdle only if EventQuery create fails. runGc retires destroyed resources.
    [[nodiscard]] bool waitGraphicsQueueFence(const char* reason, bool runGc = false);

    void createRenderPasses(bool& exposureResetRequired, caustica::rhi::CommandListHandle initializeCommandList);
    void createPostProcessRenderPasses();
    void preUpdatePathTracing(bool resetAccum, caustica::rhi::CommandListHandle commandList);
    void postUpdatePathTracing();

    PathTraceSceneBindings       m_sceneBindings;
    PathTracerScenePasses        m_scenePasses;
    CameraController             m_renderCamera;
    AccelStructManager           m_accelStructs;
    std::unique_ptr<PathTracingContext> m_pathTracingContext;
    PathTracingContext*          m_context = nullptr;

    rg::GraphBuilder             m_frameGraph;
    rg::RenderTargetPool         m_renderTargetPool;
    rg::RenderBufferPool         m_renderBufferPool;
    RenderFrameContext           m_renderFrameCtx{};

    std::unique_ptr<RtxdiPass>                  m_rtxdiPass;
    std::unique_ptr<PathTracePass>              m_pathTracePass;
    std::unique_ptr<DenoisePass>                m_denoisePass;
    std::unique_ptr<GaussianSplatFramePass>     m_gaussianFramePass;
    std::unique_ptr<RenderTargets>              m_renderTargets;
    caustica::rhi::TextureHandle                m_scratchFloat1Fallback;
    caustica::rhi::TextureHandle                m_avgLayerFallback;
    caustica::rhi::TextureHandle                m_ldrColorScratchFallback;
    caustica::rhi::BindingLayoutHandle                  m_bindingLayout;
    caustica::rhi::BindingLayoutHandle                  m_bindlessLayout;

    std::unique_ptr<caustica::rhi::CommandListPool>     m_commandListPool;
    std::unique_ptr<caustica::rhi::FrameCommandContext> m_frameCommands;
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
    FrameConstants                             m_frameConstants = {};
    bool                                        m_accumulationCompleted = false;
    bool                                        m_lastRealtimeMode = true;
    int                                         m_lastScheduledRealtimeAA = -1;

    std::vector<GaussianSplatEmissionProxy>     m_gaussianSplatEmissionProxies;
    int                                         m_gaussianSplatTemporalSampleIndex = 0;
    bool                                        m_gaussianSplatTemporalReset = true;

    // Per-frame copies from SceneRenderData (filled at render() begin).
    // During an asynchronous structure rebuild this packet combines the committed,
    // TLAS-compatible geometry with the latest light proxies. Keeping the whole
    // committed packet would otherwise leave deleted/edited lights active.
    scene::SceneRenderData                      m_frameSceneSnapshot;
    PathTracerSettings                          m_frameSettingsSnapshot;
    RenderRuntimeState                          m_frameRuntimeSnapshot;
    RenderPickState                             m_lastRenderedPicking{};
    std::atomic<uint64_t>                       m_immediateMaterialPickPosition{0};
    std::atomic<uint64_t>                       m_immediateMaterialPickRequestId{0};
    std::atomic<uint64_t>                       m_completedImmediateMaterialPickRequestId{0};
    std::atomic<uint64_t>                       m_immediateInstancePickPosition{0};
    std::atomic<uint64_t>                       m_immediateInstancePickRequestId{0};
    std::atomic<uint64_t>                       m_completedImmediateInstancePickRequestId{0};
    bool                                        m_frameGaussianSplatTemporalReset = false;

    caustica::rhi::BufferHandle                         m_feedback_Buffer_Gpu;
    caustica::rhi::BufferHandle                         m_feedback_Buffer_Cpu;
    // ADR 0002 S1: queue fence for feedback/pick CPU map (replaces device waitForIdle).
    caustica::rhi::EventQueryHandle                     m_feedbackReadbackQuery;
    bool                                                m_feedbackReadbackPending = false;
    // ADR 0002 S2: shared graphics fence for needNewPasses / RT recreate.
    caustica::rhi::EventQueryHandle                     m_graphicsSyncQuery;
    struct GpuFrameTimerSlot
    {
        caustica::rhi::TimerQueryHandle query;
        uint32_t frameIndex = 0;
        bool pending = false;
    };
    std::array<GpuFrameTimerSlot, 4>                    m_gpuFrameTimers{};
    int                                                 m_activeGpuFrameTimer = -1;
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

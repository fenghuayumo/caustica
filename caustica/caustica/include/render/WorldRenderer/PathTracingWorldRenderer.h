#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>

#include <render/Core/CameraController.h>
#include <render/WorldRenderer/PathTracingContext.h>
#include <render/WorldRenderer/PathTracingFrameExtension.h>
#include <shaders/PathTracer/Config.h>
#include <shaders/SampleConstantBuffer.h>
#include <render/Core/RenderTargets.h>
#include <render/Passes/PostProcess/PostProcess.h>
#include <render/Passes/PostProcess/AccumulationPass.h>
#include <render/Passes/Geometry/BloomPass.h>
#include <render/Passes/Denoisers/NrdIntegration.h>
#include <render/Passes/RTXDI/RtxdiPass.h>
#include <render/Passes/Debug/ShaderDebug.h>
#include <render/Passes/Gaussian/GaussianSplatEmissionProxy.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#if CAUSTICA_WITH_STREAMLINE
#include <backend/StreamlineInterface.h>
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
#include <render/Passes/Geometry/DLSS.h>
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

// =============================================================================
// PathTracingWorldRenderer — GPU path-tracing pipeline driven by PathTracingContext.
// =============================================================================
class PathTracingWorldRenderer
{
public:
    PathTracingWorldRenderer(PathTracingContext& context);
    ~PathTracingWorldRenderer();

    static nvrhi::BindingLayoutHandle CreateBindlessLayout(nvrhi::IDevice* device);
    void createBindingLayouts(nvrhi::IBindingLayout* precreatedBindless = nullptr);
    void createDeviceResources();
    void onBackBufferResizing();
    void preRender();
    void render(nvrhi::IFramebuffer* framebuffer);

    void pathTrace(nvrhi::IFramebuffer* framebuffer, const SampleConstants& constants);
    void denoise(nvrhi::IFramebuffer* framebuffer);
    void postProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset);
    void recreateBindingSet();
    void onSceneUnloading();
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
    ToneMappingPass* getToneMappingPass() { return m_toneMappingPass.get(); }

    dm::uint2 getRenderSize() const { return m_renderSize; }
    dm::uint2 getDisplaySize() const { return m_displaySize; }
    float getDisplayAspectRatio() const { return m_displayAspectRatio; }

    uint64_t getFrameIndex() const { return m_frameIndex; }
    uint getSampleIndex() const { return m_sampleIndex; }
    int getAccumulationSampleIndex() const { return m_accumulationSampleIndex; }
    bool getAccumulationCompleted() const { return m_accumulationCompleted; }

    const DebugFeedbackStruct& getFeedbackData() const { return m_feedbackData; }
    const DeltaTreeVizPathVertex* getDebugDeltaPathTree() const { return m_debugDeltaPathTree; }

    std::vector<DebugLineStruct>& getCpuSideDebugLines() { return m_cpuSideDebugLines; }

    void setGaussianSplatTemporalReset(bool v) { m_gaussianSplatTemporalReset = v; }

#if CAUSTICA_WITH_NATIVE_DLSS
    DLSS* getNativeDLSS() { return m_nativeDLSS.get(); }
#endif

private:
    [[nodiscard]] nvrhi::IDevice* device() const { return m_context.gpuDevice.GetDevice(); }

    [[nodiscard]] CameraUpdateParams makeCameraUpdateParams() const;
    void syncCameraViews();
    [[nodiscard]] dm::float2 computeCameraJitter() const;
    void dispatchFrameExtensions(PathTracingFrameEvent& event) const;

    void createRenderPasses(bool& exposureResetRequired, nvrhi::CommandListHandle initializeCommandList);
    void preUpdateLighting(nvrhi::CommandListHandle commandList, bool& needNewBindings);
    void updateLighting(nvrhi::CommandListHandle commandList);
    void preUpdatePathTracing(bool resetAccum, nvrhi::CommandListHandle commandList);
    void postUpdatePathTracing();
    void updatePathTracerConstants(PathTracerConstants& constants, const PathTracerCameraData& cameraData);
    void rtxdiSetupFrame(nvrhi::IFramebuffer* framebuffer, PathTracerCameraData cameraData, dm::uint2 renderDims);

    void postProcessPreToneMapping(nvrhi::ICommandList* commandList, const ICompositeView& compositeView);
    void postProcessPostToneMapping(nvrhi::ICommandList* commandList, const ICompositeView& compositeView);
    void renderGaussianSplats(bool renderToOutputColor);
    void accumulateGaussianSplats(const IView& splatView);

    void resetReferenceOIDN();
    void applyReferenceOIDN();
    void denoisedScreenshot(nvrhi::ITexture* framebufferTexture) const;
#if CAUSTICA_WITH_NATIVE_DLSS
    bool evaluateNativeDLSS(bool reset);
#endif

    PathTracingContext&          m_context;

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

    nvrhi::TextureHandle                        m_gaussianSplatCurrentColor;
    nvrhi::TextureHandle                        m_gaussianSplatAccumulatedColor;
    std::unique_ptr<AccumulationPass>           m_gaussianSplatAccumulationPass;
    int                                         m_gaussianSplatTemporalSampleIndex = 0;
    bool                                        m_gaussianSplatTemporalReset = true;

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

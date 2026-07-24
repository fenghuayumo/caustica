#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>
#include <shaders/PathTracer/Config.h>

#include <memory>

class AccumulationPass;
class DenoisingGuidesPass;
class NrdIntegration;
class OidnDenoiser;
class PostProcess;
class RenderTargets;
class ShaderDebug;

namespace caustica
{
class CameraController;
class ShaderFactory;
}

#if CAUSTICA_WITH_STREAMLINE
#include <backend/StreamlineInterface.h>
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
#include <render/passes/geometry/DLSS.h>
#endif

namespace caustica::render
{

class PathTracingContext;
class TemporalAntiAliasingPass;
struct FrameGraphContext;

// Denoise / AA / NRD execute helpers for the frame graph.
// Holds PathTracingContext* from create; per-frame state comes from FrameGraphContext.
class DenoisePass
{
public:
    DenoisePass();
    ~DenoisePass();

    DenoisePass(const DenoisePass&) = delete;
    DenoisePass& operator=(const DenoisePass&) = delete;

    void createGuides(
        PathTracingContext* context,
        nvrhi::IDevice* device,
        const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
        const std::unique_ptr<RenderTargets>& renderTargets,
        const std::shared_ptr<ShaderDebug>& shaderDebug,
        nvrhi::BindingLayoutHandle bindingLayout);

    // Sync per-frame handles from the graph context (replaces mega FrameBindings copy).
    void bindFrame(const FrameGraphContext& ctx);

    void prepareGuides(nvrhi::ICommandList* commandList);
    void denoiseSpecHitT(nvrhi::ICommandList* commandList);
    void computeAvgLayerRadiance(nvrhi::ICommandList* commandList);
    void stablePlanesDebugViz(nvrhi::ICommandList* commandList);
    void ensureNrdIntegrations();
    void prepareNrdInputs(nvrhi::ICommandList* commandList, int planeIndex);
    void runNrd(nvrhi::ICommandList* commandList, int planeIndex);
    void mergeNrdOutputs(nvrhi::ICommandList* commandList, int planeIndex);
    void denoiseStablePlane(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, int planeIndex);
    void denoise(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer);
    void runNoDenoiserFinalMerge(nvrhi::ICommandList* commandList);
    void runDlssUpscale(nvrhi::ICommandList* commandList, bool reset);

    void resetReferenceOIDN();
    void applyReferenceOIDN(nvrhi::ICommandList* commandList);
    void invalidateNrdIntegrations();
    void invalidateOidnOutput();

private:
#if CAUSTICA_WITH_NATIVE_DLSS
    bool evaluateNativeDLSS(nvrhi::ICommandList* commandList, bool reset);
#endif

    PathTracingContext* m_context = nullptr;
    nvrhi::IDevice* m_device = nullptr;

    // Per-frame snapshot filled by bindFrame from FrameGraphContext.
    RenderTargets* m_renderTargets = nullptr;
    PostProcess* m_postProcess = nullptr;
    nvrhi::BindingSetHandle m_bindingSet;
    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::BufferHandle m_constantBuffer;
    nvrhi::ICommandList* m_commandList = nullptr;
    dm::uint2 m_renderSize{};
    dm::uint2 m_displaySize{};
    float m_displayAspectRatio = 1.f;
    dm::float2 m_cameraJitter{};
    uint32_t m_sampleIndex = 0;
    uint64_t m_frameIndex = 0;
    int m_accumulationSampleIndex = 0;
    bool m_accumulationCompleted = false;
    int* m_gaussianSplatTemporalSampleIndex = nullptr;
    bool* m_gaussianSplatTemporalReset = nullptr;
    TemporalAntiAliasingPass* m_temporalAntiAliasing = nullptr;
    AccumulationPass* m_accumulation = nullptr;
    caustica::CameraController* m_camera = nullptr;
#if CAUSTICA_WITH_STREAMLINE
    StreamlineInterface::DLSSRROptions* m_dlssRROptions = nullptr;
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
    DLSS* m_nativeDLSS = nullptr;
#endif

    std::unique_ptr<NrdIntegration> m_nrd[cStablePlaneCount];
    std::shared_ptr<DenoisingGuidesPass> m_denoisingGuidesPass;
    std::unique_ptr<OidnDenoiser> m_oidnDenoiser;
    nvrhi::TextureHandle m_oidnDenoisedOutput;
    bool m_oidnDenoisedOutputValid = false;
    bool m_oidnDenoiserFailed = false;
};

} // namespace caustica::render

#pragma once

#include <math/math.h>
#include <rhi/rhi.h>
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
        caustica::rhi::Device* device,
        const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
        const std::unique_ptr<RenderTargets>& renderTargets,
        const std::shared_ptr<ShaderDebug>& shaderDebug,
        caustica::rhi::BindingLayoutHandle bindingLayout);

    // Sync per-frame handles from the graph context (replaces mega FrameBindings copy).
    void bindFrame(const FrameGraphContext& ctx);

    void prepareGuides(caustica::rhi::CommandList* commandList);
    void denoiseSpecHitT(caustica::rhi::CommandList* commandList);
    void computeAvgLayerRadiance(caustica::rhi::CommandList* commandList);
    void stablePlanesDebugViz(caustica::rhi::CommandList* commandList);
    void ensureNrdIntegrations();
    void prepareNrdInputs(caustica::rhi::CommandList* commandList, int planeIndex);
    void runNrd(caustica::rhi::CommandList* commandList, int planeIndex);
    void mergeNrdOutputs(caustica::rhi::CommandList* commandList, int planeIndex);
    void denoiseStablePlane(caustica::rhi::CommandList* commandList, caustica::rhi::Framebuffer* framebuffer, int planeIndex);
    void denoise(caustica::rhi::CommandList* commandList, caustica::rhi::Framebuffer* framebuffer);
    void runNoDenoiserFinalMerge(caustica::rhi::CommandList* commandList);
    void runDlssUpscale(caustica::rhi::CommandList* commandList, bool reset);

    void resetReferenceOIDN();
    void applyReferenceOIDN(caustica::rhi::CommandList* commandList);
    void invalidateNrdIntegrations();
    void invalidateOidnOutput();

private:
#if CAUSTICA_WITH_NATIVE_DLSS
    bool evaluateNativeDLSS(caustica::rhi::CommandList* commandList, bool reset);
#endif

    PathTracingContext* m_context = nullptr;
    caustica::rhi::Device* m_device = nullptr;

    // Per-frame snapshot filled by bindFrame from FrameGraphContext.
    RenderTargets* m_renderTargets = nullptr;
    PostProcess* m_postProcess = nullptr;
    caustica::rhi::BindingSetHandle m_bindingSet;
    caustica::rhi::BindingLayoutHandle m_bindingLayout;
    caustica::rhi::BufferHandle m_constantBuffer;
    caustica::rhi::CommandList* m_commandList = nullptr;
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
    caustica::rhi::TextureHandle m_oidnDenoisedOutput;
    bool m_oidnDenoisedOutputValid = false;
    bool m_oidnDenoiserFailed = false;
};

} // namespace caustica::render

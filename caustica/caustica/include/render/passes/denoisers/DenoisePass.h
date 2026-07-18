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
class GpuDevice;
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

// Denoise / AA / NRD execute helpers for the frame graph (UE RDG style).
// Owns NRD integrations and denoising-guide pass; other GPU objects are bound per frame.
class DenoisePass
{
public:
    DenoisePass();
    ~DenoisePass();

    DenoisePass(const DenoisePass&) = delete;
    DenoisePass& operator=(const DenoisePass&) = delete;

    struct FrameBindings
    {
        PathTracingContext* context = nullptr;
        nvrhi::IDevice* device = nullptr;
        RenderTargets* renderTargets = nullptr;
        PostProcess* postProcess = nullptr;
        nvrhi::BindingSetHandle bindingSet;
        nvrhi::BindingLayoutHandle bindingLayout;
        nvrhi::BufferHandle constantBuffer;
        nvrhi::ICommandList* commandList = nullptr;

        dm::uint2 renderSize{};
        dm::uint2 displaySize{};
        float displayAspectRatio = 1.f;
        dm::float2 cameraJitter{};
        uint32_t sampleIndex = 0;
        uint64_t frameIndex = 0;
        int accumulationSampleIndex = 0;
        bool accumulationCompleted = false;
        int* gaussianSplatTemporalSampleIndex = nullptr;
        bool* gaussianSplatTemporalReset = nullptr;
        TemporalAntiAliasingPass* temporalAntiAliasing = nullptr;
        AccumulationPass* accumulation = nullptr;
        CameraController* camera = nullptr;

#if CAUSTICA_WITH_STREAMLINE
        StreamlineInterface::DLSSRROptions* dlssRROptions = nullptr;
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
        DLSS* nativeDLSS = nullptr;
#endif
    };

    void createGuides(
        nvrhi::IDevice* device,
        const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
        const std::unique_ptr<RenderTargets>& renderTargets,
        const std::shared_ptr<ShaderDebug>& shaderDebug,
        nvrhi::BindingLayoutHandle bindingLayout);

    void bindFrame(const FrameBindings& bindings);

    void prepareGuides(nvrhi::ICommandList* commandList);
    void stablePlanesDebugViz(nvrhi::ICommandList* commandList);
    void ensureNrdIntegrations();
    void denoiseStablePlane(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer, int planeIndex);
    void denoise(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer);
    void runNoDenoiserFinalMerge(nvrhi::ICommandList* commandList);
    void runDlssUpscale(nvrhi::ICommandList* commandList, bool reset);

    void resetReferenceOIDN();
    void applyReferenceOIDN();
    void invalidateNrdIntegrations();
    void invalidateOidnOutput();

private:
#if CAUSTICA_WITH_NATIVE_DLSS
    bool evaluateNativeDLSS(nvrhi::ICommandList* commandList, bool reset);
#endif

    FrameBindings m_bindings{};
    std::unique_ptr<NrdIntegration> m_nrd[cStablePlaneCount];
    std::shared_ptr<DenoisingGuidesPass> m_denoisingGuidesPass;
    std::unique_ptr<OidnDenoiser> m_oidnDenoiser;
    nvrhi::TextureHandle m_oidnDenoisedOutput;
    bool m_oidnDenoisedOutputValid = false;
    bool m_oidnDenoiserFailed = false;
};

} // namespace caustica::render

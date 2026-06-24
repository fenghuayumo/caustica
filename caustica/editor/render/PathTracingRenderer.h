#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>

class PathTracerApp;
struct SampleConstants;
struct PathTracerCameraData;
struct PathTracerConstants;

namespace caustica
{
class ICompositeView;
class IView;
}

// =============================================================================
// PathTracingRenderer — Path tracing render pipeline (DeferredRenderer role).
//
// Owns the per-frame GPU orchestration extracted from PathTracerApp. Editor
// concerns (scene import, input, dropped files) remain on PathTracerApp.
//
// TODO: Relocate to causticaEngine once render resources are owned here and
//       editor coupling is reduced to IPathTracingEditorBridge callbacks.
// =============================================================================
class PathTracingRenderer
{
public:
    explicit PathTracingRenderer(PathTracerApp& app);

    void onBackBufferResizing();
    void preRender();
    void render(nvrhi::IFramebuffer* framebuffer);

    void pathTrace(nvrhi::IFramebuffer* framebuffer, const SampleConstants& constants);
    void denoise(nvrhi::IFramebuffer* framebuffer);
    void postProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset);
    void recreateBindingSet();

#if CAUSTICA_WITH_STREAMLINE
    void streamlinePreRender();
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
    void nativeDLSSPreRender();
#endif

private:
    void createRenderPasses(bool& exposureResetRequired, nvrhi::CommandListHandle initializeCommandList);
    void preUpdateLighting(nvrhi::CommandListHandle commandList, bool& needNewBindings);
    void updateLighting(nvrhi::CommandListHandle commandList);
    void preUpdatePathTracing(bool resetAccum, nvrhi::CommandListHandle commandList);
    void postUpdatePathTracing();
    void updatePathTracerConstants(PathTracerConstants& constants, const PathTracerCameraData& cameraData);
    void rtxdiSetupFrame(nvrhi::IFramebuffer* framebuffer, PathTracerCameraData cameraData, dm::uint2 renderDims);

    void postProcessPreToneMapping(nvrhi::ICommandList* commandList, const caustica::ICompositeView& compositeView);
    void postProcessPostToneMapping(nvrhi::ICommandList* commandList, const caustica::ICompositeView& compositeView);
    void renderGaussianSplats(bool renderToOutputColor);
    void accumulateGaussianSplats(const caustica::IView& splatView);

    void resetReferenceOIDN();
    void applyReferenceOIDN();
    void denoisedScreenshot(nvrhi::ITexture* framebufferTexture) const;
#if CAUSTICA_WITH_NATIVE_DLSS
    bool evaluateNativeDLSS(bool reset);
#endif

    PathTracerApp& m_app;
};

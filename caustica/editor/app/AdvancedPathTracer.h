// Path-tracing pipeline variant: RTXDI + denoiser + reference/stable-planes shader modes.
// Implements world-renderer pipeline hooks; PathTracerApp is the editor shell only.

#pragma once

#include <render/Core/PTPipelineBaker.h>
#include <render/WorldRenderer/PathTracingWorldRenderer.h>
#include <render/WorldRenderer/WorldRendererServices.h>

#include "PathTracerApp.h"

class AdvancedPathTracer : public PathTracerApp,
                           public caustica::render::IWorldRendererPipelineHooks
{
public:
    using PathTracerApp::PathTracerApp;

    void Render(nvrhi::IFramebuffer* framebuffer);
    void BackBufferResizing();

    void PathTrace(nvrhi::IFramebuffer* framebuffer, const SampleConstants& constants);
    void Denoise(nvrhi::IFramebuffer* framebuffer);
    void PostProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset);

    void SampleRenderCode(nvrhi::IFramebuffer* framebuffer, nvrhi::CommandListHandle commandList, const SampleConstants& constants);

    void CreateRTPipelines();
    void DestroyRTPipelines() override;
    std::string GetMaterialSpecializationShader() const;

    bool NeedsRasterPrecompute() { return false; }

    // IWorldRendererPipelineHooks
    bool needsRasterPrecompute() override;
    std::string getMaterialSpecializationShader() const override;
    void fillPTPipelineGlobalMacros(std::vector<caustica::ShaderMacro>& macros) override;
    void sampleRenderCode(nvrhi::IFramebuffer* framebuffer, nvrhi::CommandListHandle commandList, const SampleConstants& constants) override;
    void addCustomBindings(nvrhi::BindingSetDesc& bindingSetDesc) override;
    void createRTPipelines() override { CreateRTPipelines(); }
    void onRenderTargetsRecreated() override;
    void prepareGaussianSplatPasses() override;
    void buildGaussianSplatEmissionProxyList() override;
    bool isGaussianSplatEmissionEnabled() const override;
    bool gaussianSplatObjectsEmpty() const override;
    caustica::render::WorldRendererGaussianSplatBinding getPrimaryGaussianSplatBinding() const override;
    void renderSceneGaussianSplats(nvrhi::ICommandList* commandList,
                                   const caustica::PlanarView& splatView,
                                   RenderTargets& renderTargets,
                                   const GaussianSplatRenderSettings& settings,
                                   bool& renderedAny) override;
    void updateViews(nvrhi::IFramebuffer* framebuffer) override;
    void recreateAccelStructs(nvrhi::ICommandList* commandList) override;
    void uploadSubInstanceData(nvrhi::ICommandList* commandList) override;
    void collectUncompressedTextures() override;
    dm::float2 computeCameraJitter(uint frameIndex) override;
    bool consumeShaderReloadRequest() override;
    bool& accelerationStructRebuildRequested() override;
    bool hasActivePickRequest() const override;
    bool showDeltaTree() const override;
    bool pickMaterialRequested() const override;
    bool pickInstanceRequested() const override;
    void clearPickRequests() override;
    void resolvePickFeedback(const DebugFeedbackStruct& feedback) override;
    bool consumeExperimentalPhotoScreenshot() override;
    void captureScriptPreRender() override;
    void captureScriptPostRender(std::function<bool(const char* fileName)> saveTexture) override;
    class ZoomTool* getOrCreateZoomTool() override;
};

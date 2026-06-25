#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>
#include <shaders/SampleConstantBuffer.h>
#include <shaders/PathTracer/PathTracerDebug.hlsli>

#include <functional>
#include <render/Core/PathTracerSettings.h>
#include <render/Core/RenderCore.h>
#include <render/Passes/Gaussian/GaussianSplatEmissionProxy.h>
#include <render/Passes/Lighting/Distant/EnvMapBaker.h>
#include <render/Passes/Lighting/LightsBaker.h>
#include <render/Passes/Lighting/MaterialsBaker.h>
#include <render/Passes/OMM/OmmBaker.h>
#include <assets/cache/TextureCache.h>
#include <render/Core/DescriptorTableManager.h>
#include <engine/SceneManager.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <backend/GpuDevice.h>
#include <render/Core/View.h>

class GaussianSplatPass;
class RenderTargets;
class ComputePipelineBaker;
struct GaussianSplatRenderSettings;
struct EnvMapSceneParams;
class ZoomTool;

namespace caustica
{
class BindingCache;
class CommonRenderPasses;
class Light;
class Scene;
class ShaderFactory;
} // namespace caustica

namespace caustica::render
{

// Mutable scene/render state the world renderer reads each frame (DIVSHOT-style frame host).
struct WorldRendererHost
{
    GpuDevice* gpuDevice = nullptr;

    PathTracerSettings* settings = nullptr;
    RenderCore*       renderCore = nullptr;
    SceneManager*     sceneManager = nullptr;

    std::shared_ptr<caustica::ShaderFactory>*       shaderFactory = nullptr;
    std::shared_ptr<caustica::CommonRenderPasses>*    commonPasses = nullptr;
    std::unique_ptr<caustica::BindingCache>*        bindingCache = nullptr;
    std::shared_ptr<caustica::TextureCache>         textureCache;
    std::shared_ptr<caustica::DescriptorTableManager>* descriptorTable = nullptr;

    std::shared_ptr<EnvMapBaker>*                     envMapBaker = nullptr;
    std::shared_ptr<LightsBaker>*                     lightsBaker = nullptr;
    std::shared_ptr<MaterialsBaker>*                  materialsBaker = nullptr;
    std::shared_ptr<OmmBaker>*                        ommBaker = nullptr;
    std::shared_ptr<ComputePipelineBaker>*            computePipelineBaker = nullptr;

    std::vector<std::shared_ptr<caustica::Light>>*  lights = nullptr;
    EnvMapSceneParams*                                envMapSceneParams = nullptr;
    std::string*                                      envMapLocalPath = nullptr;
    std::string*                                      envMapOverride = nullptr;
    double*                                           sceneTime = nullptr;

    std::vector<GaussianSplatEmissionProxy>*          gaussianSplatEmissionProxies = nullptr;

    ProgressBar* progressInitializingRenderer = nullptr;
    bool*        asyncLoadingInProgress = nullptr;

    std::chrono::high_resolution_clock::time_point* benchStart = nullptr;
    std::chrono::high_resolution_clock::time_point* benchLast = nullptr;
    int* benchFrames = nullptr;
};

// Primary gaussian splat object used for binding set (editor scene owns passes).
struct WorldRendererGaussianSplatBinding
{
    const GaussianSplatPass* pass = nullptr;
    dm::float4x4             objectToWorld = dm::float4x4::identity();
};

// Editor + sample pipeline hooks implemented by AdvancedPathTracer.
class IWorldRendererPipelineHooks
{
public:
    virtual ~IWorldRendererPipelineHooks() = default;

    virtual bool needsRasterPrecompute() = 0;
    virtual std::string getMaterialSpecializationShader() const = 0;
    virtual void fillPTPipelineGlobalMacros(std::vector<caustica::ShaderMacro>& macros) = 0;
    virtual void sampleRenderCode(nvrhi::IFramebuffer* framebuffer,
                                  nvrhi::CommandListHandle commandList,
                                  const SampleConstants& constants) = 0;
    virtual void addCustomBindings(nvrhi::BindingSetDesc& bindingSetDesc) = 0;
    virtual void createRTPipelines() = 0;
    virtual void onRenderTargetsRecreated() = 0;

    virtual void prepareGaussianSplatPasses() = 0;
    virtual void buildGaussianSplatEmissionProxyList() = 0;
    virtual bool isGaussianSplatEmissionEnabled() const = 0;
    virtual bool gaussianSplatObjectsEmpty() const = 0;
    virtual WorldRendererGaussianSplatBinding getPrimaryGaussianSplatBinding() const = 0;
    virtual void renderSceneGaussianSplats(nvrhi::ICommandList* commandList,
                                           const caustica::PlanarView& splatView,
                                           RenderTargets& renderTargets,
                                           const struct GaussianSplatRenderSettings& settings,
                                           bool& renderedAny) = 0;

    virtual void updateViews(nvrhi::IFramebuffer* framebuffer) = 0;
    virtual void recreateAccelStructs(nvrhi::ICommandList* commandList) = 0;
    virtual void uploadSubInstanceData(nvrhi::ICommandList* commandList) = 0;
    virtual void collectUncompressedTextures() = 0;
    virtual dm::float2 computeCameraJitter(uint frameIndex) = 0;

    virtual bool consumeShaderReloadRequest() = 0;
    virtual bool& accelerationStructRebuildRequested() = 0;
    virtual bool hasActivePickRequest() const = 0;
    virtual bool showDeltaTree() const = 0;
    virtual bool pickMaterialRequested() const = 0;
    virtual bool pickInstanceRequested() const = 0;
    virtual void clearPickRequests() = 0;
    virtual void resolvePickFeedback(const DebugFeedbackStruct& feedback) = 0;
    virtual bool consumeExperimentalPhotoScreenshot() = 0;

    virtual void captureScriptPreRender() = 0;
    virtual void captureScriptPostRender(std::function<bool(const char* fileName)> saveTexture) = 0;

    virtual ZoomTool* getOrCreateZoomTool() = 0;
};

} // namespace caustica::render

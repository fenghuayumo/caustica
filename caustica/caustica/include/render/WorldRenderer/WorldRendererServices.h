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
#include <scene/SceneManager.h>

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

// Strong-typed dependencies wired once by Application at init (DIVSHOT Application owns renderer deps).
struct WorldRendererServices
{
    GpuDevice& gpuDevice;
    SceneManager& sceneManager;
    RenderCore& renderCore;
    PathTracerSettings& settings;

    std::shared_ptr<ShaderFactory>& shaderFactory;
    std::shared_ptr<CommonRenderPasses>& commonPasses;
    BindingCache& bindingCache;
    std::shared_ptr<TextureCache>& textureCache;
    std::shared_ptr<DescriptorTableManager>& descriptorTable;

    std::shared_ptr<EnvMapBaker>& envMapBaker;
    std::shared_ptr<LightsBaker>& lightsBaker;
    std::shared_ptr<MaterialsBaker>& materialsBaker;
    std::shared_ptr<OmmBaker>& ommBaker;
    std::shared_ptr<ComputePipelineBaker>& computePipelineBaker;

    std::vector<std::shared_ptr<caustica::Light>>& lights;
    EnvMapSceneParams& envMapSceneParams;
    std::string& envMapLocalPath;
    std::string& envMapOverride;
    double& sceneTime;

    std::vector<GaussianSplatEmissionProxy>& gaussianSplatEmissionProxies;

    ProgressBar& progressInitializingRenderer;
    bool& asyncLoadingInProgress;

    std::chrono::high_resolution_clock::time_point& benchStart;
    std::chrono::high_resolution_clock::time_point& benchLast;
    int& benchFrames;
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

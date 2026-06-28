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
#include <assets/loader/TextureLoader.h>
#include <render/Core/DescriptorTableManager.h>
#include <scene/SceneManager.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <backend/GpuDevice.h>
#include <scene/View.h>

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

struct GaussianSplatBinding
{
    const GaussianSplatPass* pass = nullptr;
    dm::float4x4             objectToWorld = dm::float4x4::identity();
};

// Editor / host callbacks wired once at init.
struct PathTracingHooks
{
    std::function<bool()>                                         needsRasterPrecompute;
    std::function<std::string()>                                  getMaterialSpecializationShader;
    std::function<void(std::vector<caustica::ShaderMacro>&)>      fillPTPipelineGlobalMacros;
    std::function<void(nvrhi::IFramebuffer*, nvrhi::CommandListHandle, const SampleConstants&)> sampleRenderCode;
    std::function<void(nvrhi::BindingSetDesc&)>                   addCustomBindings;
    std::function<void()>                                         createRTPipelines;
    std::function<void()>                                         onRenderTargetsRecreated;
    std::function<void()>                                         prepareGaussianSplatPasses;
    std::function<void()>                                         buildGaussianSplatEmissionProxyList;
    std::function<bool()>                                         isGaussianSplatEmissionEnabled;
    std::function<bool()>                                         gaussianSplatObjectsEmpty;
    std::function<GaussianSplatBinding()>                         getPrimaryGaussianSplatBinding;
    std::function<void(nvrhi::ICommandList*, const caustica::PlanarView&, RenderTargets&, const GaussianSplatRenderSettings&, bool&)> renderSceneGaussianSplats;
    std::function<void(nvrhi::IFramebuffer*)>                     updateViews;
    std::function<void(nvrhi::ICommandList*)>                     recreateAccelStructs;
    std::function<void(nvrhi::ICommandList*)>                     uploadSubInstanceData;
    std::function<void()>                                         collectUncompressedTextures;
    std::function<dm::float2(uint)>                               computeCameraJitter;
    std::function<bool()>                                         consumeShaderReloadRequest;
    std::function<bool&()>                                        accelerationStructRebuildRequested;
    std::function<bool()>                                         hasActivePickRequest;
    std::function<bool()>                                         showDeltaTree;
    std::function<bool()>                                         pickMaterialRequested;
    std::function<bool()>                                         pickInstanceRequested;
    std::function<void()>                                         clearPickRequests;
    std::function<void(const DebugFeedbackStruct&)>               resolvePickFeedback;
    std::function<bool()>                                         consumeExperimentalPhotoScreenshot;
    std::function<void()>                                         captureScriptPreRender;
    std::function<void(std::function<bool(const char*)>)>         captureScriptPostRender;
    std::function<ZoomTool*()>                                    getOrCreateZoomTool;
};

// Live references consumed by PathTracingWorldRenderer for one frame loop.
struct PathTracingContext
{
    GpuDevice& gpuDevice;
    SceneManager& sceneManager;
    RenderCore& renderCore;
    PathTracerSettings& settings;

    std::shared_ptr<ShaderFactory>& shaderFactory;
    std::shared_ptr<CommonRenderPasses>& commonPasses;
    BindingCache& bindingCache;
    std::shared_ptr<TextureLoader>& textureCache;
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

    PathTracingHooks hooks;
};

} // namespace caustica::render

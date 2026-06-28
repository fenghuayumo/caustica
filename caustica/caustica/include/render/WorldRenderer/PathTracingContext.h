#pragma once

#include <math/math.h>
#include <render/Core/PathTracerSettings.h>
#include <render/Core/RenderCore.h>
#include <render/Passes/Gaussian/GaussianSplatEmissionProxy.h>
#include <render/Passes/Lighting/Distant/EnvMapBaker.h>
#include <render/Passes/Lighting/LightsBaker.h>
#include <render/Passes/Lighting/MaterialsBaker.h>
#include <render/Passes/OMM/OmmBaker.h>
#include <render/RenderRuntimeState.h>
#include <render/WorldRenderer/PathTracingFrameExtension.h>
#include <assets/loader/TextureLoader.h>
#include <render/Core/DescriptorTableManager.h>
#include <scene/SceneManager.h>

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <backend/GpuDevice.h>

class ComputePipelineBaker;
class GaussianSplatPass;
struct EnvMapSceneParams;

namespace caustica
{
class BindingCache;
class CommonRenderPasses;
class Light;
class ShaderFactory;

namespace editor
{
class SceneGaussianSplatPasses;
class SceneLightingPasses;
class SceneRayTracingResources;
} // namespace editor

namespace render
{

struct GaussianSplatBinding
{
    const GaussianSplatPass* splatPass = nullptr;
    dm::float4x4             objectToWorld = dm::float4x4::identity();
};

// References wired once when the path tracer is created. The world renderer reads
// these every frame; scene passes are invoked directly. Optional editor tooling
// plugs in through frameExtensions at named pipeline phases.
struct PathTracingContext
{
    GpuDevice& gpuDevice;
    SceneManager& sceneManager;
    RenderCore& renderCore;
    PathTracerSettings& settings;
    RenderRuntimeState& runtimeState;

    editor::SceneRayTracingResources& rayTracing;
    editor::SceneGaussianSplatPasses& gaussianSplats;
    editor::SceneLightingPasses& lighting;

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

    std::vector<std::shared_ptr<Light>>& lights;
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

    std::span<IPathTracingFrameExtension* const> frameExtensions = {};
};

} // namespace render
} // namespace caustica

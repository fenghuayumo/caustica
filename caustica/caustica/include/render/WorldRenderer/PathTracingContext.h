#pragma once

#include <math/math.h>

#include <render/Core/PathTracerSettings.h>
#include <render/Core/RenderCore.h>
#include <render/Passes/Gaussian/GaussianSplatEmissionProxy.h>
#include <render/Passes/Lighting/Distant/EnvMapProcessor.h>
#include <render/Passes/Lighting/LightSamplingCache.h>
#include <render/Passes/Lighting/MaterialGpuCache.h>
#include <render/Passes/OMM/OpacityMicromapBuilder.h>
#include <render/RenderRuntimeState.h>
#include <render/SessionDiagnostics.h>
#include <render/WorldRenderer/PathTracingFrameExtension.h>
#include <assets/loader/TextureLoader.h>
#include <render/Core/DescriptorTableManager.h>
#include <scene/SceneManager.h>

#include <backend/GpuDevice.h>

#include <memory>
#include <span>
#include <string>
#include <vector>

class ComputePipelineRegistry;
class GaussianSplatPass;

struct EnvMapSceneParams;

namespace caustica
{
class BindingCache;
class CommonRenderPasses;
class ShaderFactory;

namespace render
{
class SceneGaussianSplatPasses;
class SceneRayTracingResources;

struct GaussianSplatBinding
{
    const GaussianSplatPass* splatPass = nullptr;
    dm::float4x4             objectToWorld = dm::float4x4::identity();
};

// References wired once when the path tracer is created. The world renderer reads
// these every frame; scene passes are invoked directly. Optional host tooling
// plugs in through frameExtensions at named pipeline phases.
struct PathTracingContext
{
    GpuDevice& gpuDevice;
    SceneManager& sceneManager;
    RenderCore& renderCore;
    PathTracerSettings& settings;
    RenderRuntimeState& runtimeState;

    SceneRayTracingResources& rayTracing;
    SceneGaussianSplatPasses& gaussianSplats;

    std::shared_ptr<ShaderFactory>& shaderFactory;
    std::shared_ptr<CommonRenderPasses>& commonPasses;
    BindingCache& bindingCache;
    std::shared_ptr<TextureLoader>& textureCache;
    std::shared_ptr<DescriptorTableManager>& descriptorTable;

    std::shared_ptr<EnvMapProcessor>& environment;
    std::shared_ptr<LightSamplingCache>& lightSampling;
    std::shared_ptr<MaterialGpuCache>& materials;
    std::shared_ptr<OpacityMicromapBuilder>& opacityMaps;
    std::shared_ptr<ComputePipelineRegistry>& computePipelines;

    EnvMapSceneParams& envMapSceneParams;
    std::string& envMapLocalPath;
    std::string& envMapOverride;
    double& sceneTime;

    std::vector<GaussianSplatEmissionProxy>& gaussianSplatEmissionProxies;

    SessionDiagnostics& diagnostics;

    std::span<IPathTracingFrameExtension* const> frameExtensions = {};
};

} // namespace render
} // namespace caustica

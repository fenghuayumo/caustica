#pragma once

#include <render/core/PathTracerSettings.h>
#include <render/SceneGpuResources.h>
#include <rhi/rhi.h>
#include <math/math.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class EnvMapProcessor;
class LightSamplingCache;
class MaterialGpuCache;
class OpacityMicromapBuilder;
struct EnvMapSceneParams;
struct GaussianSplatEmissionProxy;

namespace caustica
{
class AccelStructManager;
class BindingCache;
class CameraController;
namespace render { class RenderDevice; }
namespace scene { class SceneRenderData; }

struct PreUpdateLightingParams
{
    caustica::rhi::ICommandList*                         commandList = nullptr;
    bool&                                          needNewBindings;

    EnvMapProcessor*                             environment = nullptr;
    caustica::render::RenderDevice&                             renderDevice;

    std::string                                    envMapActualPath;
    std::filesystem::path                          sceneDirectory;
};

struct UpdateLightingParams
{
    PathTracerSettings&                            settings;

    caustica::rhi::ICommandList*                           commandList = nullptr;
    EnvMapProcessor*                             environment = nullptr;
    LightSamplingCache*                          lightSampling = nullptr;
    BindingCache*                                  bindingCache = nullptr;
    caustica::render::RenderDevice&                             renderDevice;

    const scene::SceneRenderData*                  sceneData = nullptr;
    render::SceneGpuFrameHandles                   gpuHandles{};
    caustica::rhi::IDescriptorTable*                       bindlessDescriptorTable = nullptr;
    std::shared_ptr<MaterialGpuCache>              materials;
    std::shared_ptr<OpacityMicromapBuilder>        opacityMaps;

    EnvMapSceneParams&                             envMapSceneParams;
    double                                         sceneTime = 0.0;
    uint64_t                                       frameIndex = 0;

    float                                          envMapRadianceScale = 0.25f;

    const std::vector<GaussianSplatEmissionProxy>*   gaussianSplatEmissionProxies = nullptr;
};

struct UpdateLightingEndParams
{
    caustica::rhi::ICommandList*                           commandList = nullptr;
    LightSamplingCache*                          lightSampling = nullptr;
    BindingCache*                                  bindingCache = nullptr;

    render::SceneGpuFrameHandles                   gpuHandles{};
    std::shared_ptr<MaterialGpuCache>              materials;
    std::shared_ptr<OpacityMicromapBuilder>        opacityMaps;

    caustica::rhi::BufferHandle                            subInstanceDataBuffer;
    caustica::rhi::TextureHandle                           depthBuffer;
    caustica::rhi::TextureHandle                           motionVectors;
};

void syncEnvMapSceneParams(
    const PathTracerSettings& settings,
    EnvMapSceneParams& params,
    float envMapRadianceScale);

void preUpdateLighting(PreUpdateLightingParams& params);

// EnvMap bake / importance (no LightSampling). May set ResetAccumulation.
void updateEnvMapLighting(UpdateLightingParams& params);

// Light proxy / emissive bake begin. Requires EnvMap importance map to be current.
void updateLightSamplingBegin(
    CameraController& camera,
    AccelStructManager& accelStructs,
    UpdateLightingParams& params);

// Convenience: EnvMap then LightSampling begin (legacy callers).
void updateLighting(
    CameraController& camera,
    AccelStructManager& accelStructs,
    UpdateLightingParams& params);

void updateLightingEnd(UpdateLightingEndParams& params);

} // namespace caustica

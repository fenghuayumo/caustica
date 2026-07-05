#pragma once

#include <render/Core/PathTracerSettings.h>
#include <rhi/nvrhi.h>
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
class Light;
class Scene;

struct PreUpdateLightingParams
{
    nvrhi::ICommandList*                         commandList = nullptr;
    bool&                                          needNewBindings;

    EnvMapProcessor*                             environment = nullptr;
    caustica::render::RenderDevice&                             renderDevice;

    std::string                                    envMapActualPath;
    std::filesystem::path                          sceneDirectory;
};

struct UpdateLightingParams
{
    PathTracerSettings&                            settings;

    nvrhi::ICommandList*                           commandList = nullptr;
    EnvMapProcessor*                             environment = nullptr;
    LightSamplingCache*                          lightSampling = nullptr;
    BindingCache*                                  bindingCache = nullptr;
    caustica::render::RenderDevice&                             renderDevice;

    const std::shared_ptr<Scene>&                  scene;
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
    nvrhi::ICommandList*                           commandList = nullptr;
    LightSamplingCache*                          lightSampling = nullptr;
    BindingCache*                                  bindingCache = nullptr;

    const std::shared_ptr<Scene>&                  scene;
    std::shared_ptr<MaterialGpuCache>              materials;
    std::shared_ptr<OpacityMicromapBuilder>        opacityMaps;

    nvrhi::BufferHandle                            subInstanceDataBuffer;
    nvrhi::TextureHandle                           depthBuffer;
    nvrhi::TextureHandle                           motionVectors;
};

void syncEnvMapSceneParams(
    const PathTracerSettings& settings,
    EnvMapSceneParams& params,
    float envMapRadianceScale);

void preUpdateLighting(PreUpdateLightingParams& params);

void updateLighting(
    CameraController& camera,
    AccelStructManager& accelStructs,
    UpdateLightingParams& params);

void updateLightingEnd(UpdateLightingEndParams& params);

} // namespace caustica

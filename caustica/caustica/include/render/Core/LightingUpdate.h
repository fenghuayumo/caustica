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
class BindingCache;
class CommonRenderPasses;
class Light;
class Scene;

struct PreUpdateLightingParams
{
    nvrhi::ICommandList*                         commandList = nullptr;
    bool&                                          needNewBindings;

    EnvMapProcessor*                                   envMapProcessor = nullptr;
    std::shared_ptr<CommonRenderPasses>            commonPasses;

    std::string                                    envMapActualPath;
    std::filesystem::path                          sceneDirectory;
};

struct UpdateLightingParams
{
    PathTracerSettings&                            settings;

    nvrhi::ICommandList*                           commandList = nullptr;
    EnvMapProcessor*                                   envMapProcessor = nullptr;
    LightSamplingCache*                                   lightSamplingCache = nullptr;
    BindingCache*                                  bindingCache = nullptr;
    std::shared_ptr<CommonRenderPasses>            commonPasses;

    const std::vector<std::shared_ptr<Light>>*     lights = nullptr;
    const std::shared_ptr<Scene>&                  scene;
    std::shared_ptr<MaterialGpuCache>                materialGpuCache;
    std::shared_ptr<OpacityMicromapBuilder>                        opacityMicromapBuilder;

    EnvMapSceneParams&                             envMapSceneParams;
    double                                         sceneTime = 0.0;
    uint64_t                                       frameIndex = 0;

    float                                          envMapRadianceScale = 0.25f;

    const std::vector<GaussianSplatEmissionProxy>*   gaussianSplatEmissionProxies = nullptr;
};

struct UpdateLightingEndParams
{
    nvrhi::ICommandList*                           commandList = nullptr;
    LightSamplingCache*                                   lightSamplingCache = nullptr;
    BindingCache*                                  bindingCache = nullptr;

    const std::shared_ptr<Scene>&                  scene;
    std::shared_ptr<MaterialGpuCache>                materialGpuCache;
    std::shared_ptr<OpacityMicromapBuilder>                        opacityMicromapBuilder;

    nvrhi::BufferHandle                            subInstanceDataBuffer;
    nvrhi::TextureHandle                           depthBuffer;
    nvrhi::TextureHandle                           motionVectors;
};

void syncEnvMapSceneParams(
    const PathTracerSettings& settings,
    EnvMapSceneParams& params,
    float envMapRadianceScale);

} // namespace caustica

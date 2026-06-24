#pragma once

#include <render/Core/PathTracerSettings.h>
#include <rhi/nvrhi.h>
#include <math/math.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class EnvMapBaker;
class LightsBaker;
class MaterialsBaker;
class OmmBaker;
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

    EnvMapBaker*                                   envMapBaker = nullptr;
    std::shared_ptr<CommonRenderPasses>            commonPasses;

    std::string                                    envMapActualPath;
    std::filesystem::path                          sceneDirectory;
};

struct UpdateLightingParams
{
    PathTracerSettings&                            settings;

    nvrhi::ICommandList*                           commandList = nullptr;
    EnvMapBaker*                                   envMapBaker = nullptr;
    LightsBaker*                                   lightsBaker = nullptr;
    BindingCache*                                  bindingCache = nullptr;
    std::shared_ptr<CommonRenderPasses>            commonPasses;

    const std::vector<std::shared_ptr<Light>>*     lights = nullptr;
    const std::shared_ptr<Scene>&                  scene;
    std::shared_ptr<MaterialsBaker>                materialsBaker;
    std::shared_ptr<OmmBaker>                        ommBaker;

    EnvMapSceneParams&                             envMapSceneParams;
    double                                         sceneTime = 0.0;
    uint64_t                                       frameIndex = 0;

    float                                          envMapRadianceScale = 0.25f;

    const std::vector<GaussianSplatEmissionProxy>*   gaussianSplatEmissionProxies = nullptr;
};

} // namespace caustica

#include <render/passes/lighting/LightingFrame.h>

#include <render/PathTracingContext.h>
#include <render/core/LightingUpdate.h>
#include <render/core/PathTracerSettings.h>
#include <render/passes/debug/ShaderDebug.h>
#include <render/passes/gaussian/GaussianSplatEmissionProxy.h>
#include <render/passes/lighting/LightSamplingCache.h>
#include <render/passes/lighting/distant/EnvMapImportanceSamplingCache.h>
#include <render/passes/lighting/distant/EnvMapProcessor.h>
#include <core/path_utils.h>
#include <scene/scene_utils.h>

#include <filesystem>
#include <string>

using namespace caustica;
using namespace caustica::math;
using namespace caustica::render;

namespace
{
    constexpr float c_envMapRadianceScale = 1.0f / 4.0f;
}

void caustica::render::createLightingRenderPasses(
    PathTracingContext& context,
    caustica::rhi::IDevice* device,
    const std::shared_ptr<ShaderDebug>& shaderDebug,
    caustica::rhi::BindingLayoutHandle bindlessLayout,
    caustica::rhi::CommandListHandle initializeCommandList,
    dm::uint2 screenResolution)
{
    if (context.scenePasses.lighting.environment() == nullptr)
        context.scenePasses.lighting.environment() = std::make_shared<EnvMapProcessor>(device, context.textureCache, false);
    if (context.scenePasses.lighting.lightSampling() == nullptr)
        context.scenePasses.lighting.lightSampling() = std::make_shared<LightSamplingCache>(device);

    context.scenePasses.lighting.environment()->createRenderPasses(
        shaderDebug, context.shaderFactory, context.scenePasses.lighting.computePipelines());
    context.scenePasses.lighting.environment()->generateBRDFLUT(
        initializeCommandList.Get(), context.bindingCache);
    context.scenePasses.lighting.lightSampling()->createRenderPasses(
        context.shaderFactory,
        bindlessLayout,
        context.renderDevice,
        shaderDebug,
        screenResolution,
        context.scenePasses.lighting.environment()->getImportanceSampling()->getImportanceMapResolution());
}

void caustica::render::preUpdateLightingFrame(
    PathTracingContext& context,
    caustica::rhi::CommandListHandle commandList,
    bool& needNewBindings)
{
    std::filesystem::path sceneDirectory;
    if (!isInlineScenePath(context.sessionScenePath))
        sceneDirectory = context.sessionScenePath.parent_path();

    std::string envMapActualPath = context.scenePasses.lighting.envMapLocalPath();
    if (context.scenePasses.lighting.envMapOverride() != ""
        && context.scenePasses.lighting.envMapOverride() != c_EnvMapSceneDefault)
    {
        envMapActualPath = isProceduralSky(context.scenePasses.lighting.envMapOverride().c_str())
            ? context.scenePasses.lighting.envMapOverride()
            : (std::string(c_EnvMapSubFolder) + "/" + context.scenePasses.lighting.envMapOverride());
    }

    if (!envMapActualPath.empty() && !isProceduralSky(envMapActualPath.c_str()))
        envMapActualPath = resolveSceneMediaPath(envMapActualPath, sceneDirectory).generic_string();

    PreUpdateLightingParams params{
        commandList,
        needNewBindings,
        context.scenePasses.lighting.environment().get(),
        context.renderDevice,
        envMapActualPath,
        sceneDirectory,
    };
    caustica::preUpdateLighting(params);
}

void caustica::render::updateLightingFrame(
    PathTracingContext& context,
    caustica::rhi::CommandListHandle commandList,
    uint64_t frameIndex,
    const std::vector<GaussianSplatEmissionProxy>* gaussianSplatEmissionProxies)
{
    UpdateLightingParams params{
        .settings = context.activeSettings(),
        .commandList = commandList,
        .environment = context.scenePasses.lighting.environment().get(),
        .lightSampling = context.scenePasses.lighting.lightSampling().get(),
        .bindingCache = &context.bindingCache,
        .renderDevice = context.renderDevice,
        .sceneData = context.frameScene,
        .gpuHandles = context.resolveGpuHandles(),
        .bindlessDescriptorTable = context.descriptorTable
            ? context.descriptorTable->getDescriptorTable()
            : nullptr,
        .materials = context.scenePasses.lighting.materials(),
        .opacityMaps = context.scenePasses.lighting.opacityMaps(),
        .envMapSceneParams = context.scenePasses.lighting.envMapSceneParams(),
        .sceneTime = context.sceneTime,
        .frameIndex = frameIndex,
        .envMapRadianceScale = c_envMapRadianceScale,
    };
    if (gaussianSplatEmissionProxies && !gaussianSplatEmissionProxies->empty())
        params.gaussianSplatEmissionProxies = gaussianSplatEmissionProxies;
    caustica::updateLighting(context.camera, context.accelStructs, params);
}

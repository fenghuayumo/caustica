#include <render/WorldRenderer.h>

#include <render/PathTracingContext.h>
#include <render/core/LightingUpdate.h>
#include <render/core/PathTracerSettings.h>
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

void caustica::render::WorldRenderer::createLightingRenderPasses(nvrhi::CommandListHandle initializeCommandList)
{
    const uint2 screenResolution = {m_renderTargets->outputColor->getDesc().width, m_renderTargets->outputColor->getDesc().height};

    if (m_context->scenePasses.lighting.environment() == nullptr)
        m_context->scenePasses.lighting.environment() = std::make_shared<EnvMapProcessor>(device(), m_context->textureCache, false);
    if (m_context->scenePasses.lighting.lightSampling() == nullptr)
        m_context->scenePasses.lighting.lightSampling() = std::make_shared<LightSamplingCache>(device());
    m_context->scenePasses.lighting.environment()->createRenderPasses(m_shaderDebug, m_context->shaderFactory, m_context->scenePasses.lighting.computePipelines());
    m_context->scenePasses.lighting.environment()->generateBRDFLUT(initializeCommandList.Get(), m_context->bindingCache);  // One-time BRDF LUT generation
    m_context->scenePasses.lighting.lightSampling()->createRenderPasses(m_context->shaderFactory, m_bindlessLayout, m_context->renderDevice, m_shaderDebug, screenResolution, m_context->scenePasses.lighting.environment()->getImportanceSampling()->getImportanceMapResolution());
}

void caustica::render::WorldRenderer::preUpdateLighting(nvrhi::CommandListHandle commandList, bool& needNewBindings)
{
    std::filesystem::path sceneDirectory;
    if (!isInlineScenePath(m_context->sessionScenePath))
        sceneDirectory = m_context->sessionScenePath.parent_path();

    std::string envMapActualPath = m_context->scenePasses.lighting.envMapLocalPath();
    if (m_context->scenePasses.lighting.envMapOverride() != "" && m_context->scenePasses.lighting.envMapOverride() != c_EnvMapSceneDefault)
        envMapActualPath = (isProceduralSky(m_context->scenePasses.lighting.envMapOverride().c_str())) ? (m_context->scenePasses.lighting.envMapOverride()) : (std::string(c_EnvMapSubFolder) + "/" + m_context->scenePasses.lighting.envMapOverride());

    if (!envMapActualPath.empty() && !isProceduralSky(envMapActualPath.c_str()))
        envMapActualPath = resolveSceneMediaPath(envMapActualPath, sceneDirectory).generic_string();

    PreUpdateLightingParams params{
        commandList,
        needNewBindings,
        m_context->scenePasses.lighting.environment().get(),
        m_context->renderDevice,
        envMapActualPath,
        sceneDirectory,
    };
    caustica::preUpdateLighting(params);
}

void caustica::render::WorldRenderer::updateLighting(nvrhi::CommandListHandle commandList)
{
    buildGaussianSplatEmissionProxies();

    UpdateLightingParams params{
        .settings = m_context->activeSettings(),
        .commandList = commandList,
        .environment = m_context->scenePasses.lighting.environment().get(),
        .lightSampling = m_context->scenePasses.lighting.lightSampling().get(),
        .bindingCache = &m_context->bindingCache,
        .renderDevice = m_context->renderDevice,
        .sceneData = m_context->frameScene,
        .gpuHandles = m_context->resolveGpuHandles(),
        .bindlessDescriptorTable = m_context->descriptorTable
            ? m_context->descriptorTable->getDescriptorTable()
            : nullptr,
        .materials = m_context->scenePasses.lighting.materials(),
        .opacityMaps = m_context->scenePasses.lighting.opacityMaps(),
        .envMapSceneParams = m_context->scenePasses.lighting.envMapSceneParams(),
        .sceneTime = m_context->sceneTime,
        .frameIndex = m_frameIndex,
        .envMapRadianceScale = c_envMapRadianceScale,
    };
    if (!m_gaussianSplatEmissionProxies.empty())
        params.gaussianSplatEmissionProxies = &m_gaussianSplatEmissionProxies;
    caustica::updateLighting(m_context->camera, m_context->accelStructs, params);
}

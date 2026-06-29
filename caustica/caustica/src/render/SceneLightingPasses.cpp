#include <render/SceneLightingPasses.h>

#include <assets/loader/ShaderFactory.h>
#include <core/path_utils.h>
#include <render/Core/ComputePipelineRegistry.h>
#include <render/Passes/OMM/OpacityMicromapBuilder.h>
#include <scene/scene_utils.h>
#include <scene/SceneGraph.h>
#include <scene/Scene.h>
#include <shaders/light_cb.h>

#include <math/math.h>

namespace caustica::render
{

void SceneLightingPasses::refreshEnvironmentMapMediaList(const std::filesystem::path& assetsFolder,
    const std::filesystem::path& currentScenePath)
{
    SceneManager::refreshEnvironmentMapMediaList(
        assetsFolder,
        c_EnvMapSubFolder,
        currentScenePath,
        m_envMapMediaList,
        m_envMapMediaFolder);
}

void SceneLightingPasses::setEnvMapOverrideSource(const std::string& envMapOverride)
{
    if (m_envMapOverride != envMapOverride && m_envMapProcessor != nullptr)
        m_envMapProcessor->SetTargetCubeResolution(0);
    m_envMapOverride = envMapOverride;
}

void SceneLightingPasses::createOpacityMicromapBuilderIfSupported(nvrhi::IDevice* device,
    const std::shared_ptr<caustica::DescriptorTableManager>& descriptorTable,
    const std::shared_ptr<caustica::TextureLoader>& textureLoader,
    const std::shared_ptr<caustica::ShaderFactory>& shaderFactory)
{
    if (device != nullptr && device->queryFeatureSupport(nvrhi::Feature::RayTracingOpacityMicromap))
        m_opacityMicromapBuilder = std::make_shared<OpacityMicromapBuilder>(device, descriptorTable, textureLoader, shaderFactory);
}

void SceneLightingPasses::sceneUnloading()
{
    m_lights.clear();
    m_envMapProcessor = nullptr;
    m_lightSamplingCache = nullptr;
    m_materialGpuCache = nullptr;
    if (m_opacityMicromapBuilder != nullptr)
        m_opacityMicromapBuilder->SceneUnloading();
    m_computePipelineRegistry = nullptr;
}

void SceneLightingPasses::onSceneLoaded(caustica::Scene& scene, PathTracerSettings& settings)
{
    auto sceneGraph = scene.GetSceneGraph();
    if (sceneGraph == nullptr)
        return;

    m_lights.clear();
    for (auto light : sceneGraph->GetLights())
        m_lights.push_back(light);

    std::shared_ptr<EnvironmentLight> envLight = FindEnvironmentLight(m_lights);
    m_envMapLocalPath = (envLight == nullptr) ? ("") : (envLight->path);
    settings.EnvironmentMapParams = EnvironmentMapRuntimeParameters();
    m_envMapOverride = c_EnvMapSceneDefault;

    for (int i = static_cast<int>(m_lights.size()) - 1; i >= 0; --i)
    {
        LightConstants lc;
        m_lights[i]->FillLightConstants(lc);
        if (length(lc.color * lc.intensity) <= 1e-7f)
            m_lights.erase(m_lights.begin() + i);
    }

    if (m_envMapLocalPath != "")
    {
        if (envLight == nullptr)
        {
            envLight = std::make_shared<EnvironmentLight>();
            sceneGraph->AttachLeafNode(sceneGraph->GetRootNode(), envLight);
            m_lights.push_back(envLight);
        }
    }
}

void SceneLightingPasses::resyncLightsFromSceneGraph(SceneGraph& sceneGraph)
{
    m_lights.clear();
    for (auto light : sceneGraph.GetLights())
        m_lights.push_back(light);
}

void SceneLightingPasses::notifyGpuCachesSceneReloaded(caustica::Scene& scene)
{
    if (m_materialGpuCache != nullptr)
        m_materialGpuCache->SceneReloaded();
    if (m_envMapProcessor != nullptr)
        m_envMapProcessor->SceneReloaded();
    if (m_lightSamplingCache != nullptr)
        m_lightSamplingCache->SceneReloaded();
    if (m_opacityMicromapBuilder != nullptr)
        m_opacityMicromapBuilder->SceneLoaded(scene);
}

void SceneLightingPasses::applyGpuCacheShaderMacros(std::vector<caustica::ShaderMacro>& macros)
{
    if (m_lightSamplingCache != nullptr)
        m_lightSamplingCache->SetGlobalShaderMacros(macros);
    if (m_opacityMicromapBuilder != nullptr)
        m_opacityMicromapBuilder->SetGlobalShaderMacros(macros);
}

void SceneLightingPasses::createOpacityMicromaps(caustica::Scene& scene)
{
    if (m_opacityMicromapBuilder != nullptr)
        m_opacityMicromapBuilder->CreateOpacityMicromaps(scene);
}

void SceneLightingPasses::forEachUsedMaterialTexture(
    const std::function<void(std::shared_ptr<caustica::LoadedTexture>, bool normalMap)>& visitor)
{
    if (m_materialGpuCache == nullptr)
        return;

    for (auto textureIT : m_materialGpuCache->GetUsedTextures())
        visitor(textureIT.second.Loaded, textureIT.second.NormalMap);
}

} // namespace caustica::render

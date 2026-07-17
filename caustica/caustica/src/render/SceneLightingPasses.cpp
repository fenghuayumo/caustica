#include <render/SceneLightingPasses.h>

#include <assets/loader/ShaderFactory.h>
#include <core/path_utils.h>
#include <render/core/ComputePipelineRegistry.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <scene/scene_utils.h>
#include <scene/Scene.h>
#include <scene/SceneLightAccess.h>
#include <scene/SceneRenderData.h>
#include <scene/ResourceTracker.h>
#include <scene/SceneTypes.h>

namespace caustica::render
{

void SceneLightingPasses::refreshEnvironmentMapMediaList(const std::filesystem::path& assetsFolder,
    const std::filesystem::path& currentScenePath)
{
    caustica::refreshEnvironmentMapMediaList(
        assetsFolder,
        c_EnvMapSubFolder,
        currentScenePath,
        m_envMapMediaList,
        m_envMapMediaFolder);
}

void SceneLightingPasses::setEnvMapOverrideSource(const std::string& envMapOverride)
{
    if (m_envMapOverride != envMapOverride && m_environment != nullptr)
        m_environment->setTargetCubeResolution(0);
    m_envMapOverride = envMapOverride;
}

void SceneLightingPasses::createOpacityMapsIfSupported(nvrhi::IDevice* device,
    const std::shared_ptr<caustica::DescriptorTableManager>& descriptorTable,
    const std::shared_ptr<caustica::TextureLoader>& textureLoader,
    const std::shared_ptr<caustica::ShaderFactory>& shaderFactory)
{
    if (device != nullptr && device->queryFeatureSupport(nvrhi::Feature::RayTracingOpacityMicromap))
        m_opacityMaps = std::make_shared<OpacityMicromapBuilder>(device, descriptorTable, textureLoader, shaderFactory);
}

void SceneLightingPasses::sceneUnloading()
{
    m_environment = nullptr;
    m_lightSampling = nullptr;
    m_materials = nullptr;
    if (m_opacityMaps != nullptr)
        m_opacityMaps->sceneUnloading();
    m_computePipelines = nullptr;
}

void SceneLightingPasses::onSceneLoaded(const caustica::scene::SceneRenderData& renderData, PathTracerSettings& settings)
{
    // Prefer published light proxies (refreshAfterLoad publishes before this runs).
    m_envMapLocalPath.clear();
    for (const scene::LightRenderProxy& light : renderData.lights)
    {
        if (const scene::EnvironmentLightData* env = scene::tryGetEnvironmentLightData(light.data))
        {
            m_envMapLocalPath = env->path;
            break;
        }
    }

    settings.EnvironmentMapParams = EnvironmentMapRuntimeParameters();
    m_envMapOverride = c_EnvMapSceneDefault;
}

void SceneLightingPasses::notifySceneReloaded(size_t geometryCount)
{
    if (m_materials != nullptr)
        m_materials->sceneReloaded();
    if (m_environment != nullptr)
        m_environment->sceneReloaded();
    if (m_lightSampling != nullptr)
        m_lightSampling->sceneReloaded();
    if (m_opacityMaps != nullptr)
        m_opacityMaps->sceneLoaded(geometryCount);
}

int SceneLightingPasses::ensureMaterialsFromScene(
    const caustica::scene::SceneRenderData& renderData)
{
    if (m_materials == nullptr)
        return 0;
    return m_materials->ensureMaterialsFromScene(renderData.materialResources);
}

void SceneLightingPasses::applyShaderMacros(std::vector<caustica::ShaderMacro>& macros)
{
    if (m_lightSampling != nullptr)
        m_lightSampling->setGlobalShaderMacros(macros);
    if (m_opacityMaps != nullptr)
        m_opacityMaps->setGlobalShaderMacros(macros);
}

void SceneLightingPasses::createOpacityMicromaps(
    std::span<const std::shared_ptr<caustica::MeshInfo>> meshes,
    size_t geometryCount)
{
    if (m_opacityMaps != nullptr)
        m_opacityMaps->createOpacityMicromaps(meshes, geometryCount);
}

void SceneLightingPasses::forEachUsedMaterialTexture(
    const std::function<void(caustica::Handle<caustica::ImageAsset>, bool normalMap)>& visitor)
{
    if (m_materials == nullptr)
        return;

    for (auto textureIT : m_materials->getUsedTextures())
        visitor(textureIT.second.loaded, textureIT.second.normalMap);
}

} // namespace caustica::render

#include <render/SceneLightingPasses.h>

#include <assets/loader/ShaderFactory.h>
#include <core/path_utils.h>
#include <render/core/ComputePipelineRegistry.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <scene/scene_utils.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>
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
    if (m_envMapOverride != envMapOverride && m_environment != nullptr)
        m_environment->SetTargetCubeResolution(0);
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
    m_lightEntities.clear();
    m_environment = nullptr;
    m_lightSampling = nullptr;
    m_materials = nullptr;
    if (m_opacityMaps != nullptr)
        m_opacityMaps->SceneUnloading();
    m_computePipelines = nullptr;
}

void SceneLightingPasses::onSceneLoaded(caustica::Scene& scene, PathTracerSettings& settings)
{
    m_lightEntities.clear();
    for (ecs::Entity lightEntity : scene.GetLightEntities())
        m_lightEntities.push_back(lightEntity);

    const scene::SceneEntityWorld* entityWorld = scene.GetEntityWorld();
    ecs::Entity envEntity = FindEnvironmentLightEntity(scene);
    m_envMapLocalPath = "";
    if (entityWorld && ecs::isValid(envEntity))
    {
        if (const auto* light = scene::TryGetLight(entityWorld->world(), envEntity))
            m_envMapLocalPath = scene::GetEnvironmentLightPath(*light);
    }
    settings.EnvironmentMapParams = EnvironmentMapRuntimeParameters();
    m_envMapOverride = c_EnvMapSceneDefault;

    if (entityWorld)
    {
        auto& world = entityWorld->world();
        for (int i = static_cast<int>(m_lightEntities.size()) - 1; i >= 0; --i)
        {
            const auto* light = scene::TryGetLight(world, m_lightEntities[i]);
            const auto* global = world.tryGet<scene::GlobalTransformComponent>(m_lightEntities[i]);
            if (!light || !global)
                continue;

            LightConstants lc;
            scene::FillLightConstants(*light, global->transform, lc);
            if (length(lc.color * lc.intensity) <= 1e-7f)
                m_lightEntities.erase(m_lightEntities.begin() + i);
        }
    }

    if (m_envMapLocalPath != "")
    {
        if (!ecs::isValid(envEntity))
        {
            scene::LightComponent component;
            component.data = scene::EnvironmentLightData{};
            if (auto* environment = std::get_if<scene::EnvironmentLightData>(&component.data))
                environment->path = m_envMapLocalPath;
            scene.AttachLightToRoot(std::move(component), "Environment");
            m_lightEntities.push_back(scene.GetLightEntities().back());
        }
    }
}

void SceneLightingPasses::resyncLightsFromScene(caustica::Scene& scene)
{
    m_lightEntities.clear();
    auto* entityWorld = scene.GetEntityWorld();
    for (ecs::Entity lightEntity : scene.GetLightEntities())
    {
        if (entityWorld && !entityWorld->world().isAlive(lightEntity))
            continue;
        m_lightEntities.push_back(lightEntity);
    }
}

void SceneLightingPasses::notifySceneReloaded(caustica::Scene& scene)
{
    if (m_materials != nullptr)
        m_materials->sceneReloaded();
    if (m_environment != nullptr)
        m_environment->SceneReloaded();
    if (m_lightSampling != nullptr)
        m_lightSampling->SceneReloaded();
    if (m_opacityMaps != nullptr)
        m_opacityMaps->SceneLoaded(scene);
}

int SceneLightingPasses::ensureMaterialsFromScene(const std::shared_ptr<caustica::Scene>& scene)
{
    if (m_materials == nullptr || !scene)
        return 0;
    return m_materials->ensureMaterialsFromScene(scene);
}

void SceneLightingPasses::applyShaderMacros(std::vector<caustica::ShaderMacro>& macros)
{
    if (m_lightSampling != nullptr)
        m_lightSampling->SetGlobalShaderMacros(macros);
    if (m_opacityMaps != nullptr)
        m_opacityMaps->SetGlobalShaderMacros(macros);
}

void SceneLightingPasses::createOpacityMicromaps(caustica::Scene& scene)
{
    if (m_opacityMaps != nullptr)
        m_opacityMaps->CreateOpacityMicromaps(scene);
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

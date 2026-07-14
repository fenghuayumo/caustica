#pragma once

#include <render/passes/lighting/distant/EnvMapProcessor.h>
#include <render/passes/lighting/LightSamplingCache.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <shaders/PathTracer/Lighting/EnvMap.hlsli>

#include <assets/loader/TextureLoader.h>
#include <ecs/Entity.h>
#include <render/core/DescriptorTableManager.h>
#include <render/core/PathTracerSettings.h>
#include <scene/SceneManager.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ComputePipelineRegistry;

namespace caustica
{
class Scene;
class ShaderFactory;
} // namespace caustica

namespace caustica::render
{

// Scene-scoped lighting: GPU caches/builders, analytic lights, and env-map selection.
class SceneLightingPasses
{
public:
    std::shared_ptr<MaterialGpuCache>& materials() { return m_materials; }
    const std::shared_ptr<MaterialGpuCache>& materials() const { return m_materials; }
    std::shared_ptr<LightSamplingCache>& lightSampling() { return m_lightSampling; }
    const std::shared_ptr<LightSamplingCache>& lightSampling() const { return m_lightSampling; }
    std::shared_ptr<EnvMapProcessor>& environment() { return m_environment; }
    const std::shared_ptr<EnvMapProcessor>& environment() const { return m_environment; }
    std::shared_ptr<OpacityMicromapBuilder>& opacityMaps() { return m_opacityMaps; }
    const std::shared_ptr<OpacityMicromapBuilder>& opacityMaps() const { return m_opacityMaps; }
    std::shared_ptr<ComputePipelineRegistry>& computePipelines() { return m_computePipelines; }
    const std::shared_ptr<ComputePipelineRegistry>& computePipelines() const { return m_computePipelines; }

    std::vector<ecs::Entity>& lightEntities() { return m_lightEntities; }
    const std::vector<ecs::Entity>& lightEntities() const { return m_lightEntities; }

    EnvMapSceneParams& envMapSceneParams() { return m_envMapSceneParams; }
    const EnvMapSceneParams& envMapSceneParams() const { return m_envMapSceneParams; }

    std::string& envMapLocalPath() { return m_envMapLocalPath; }
    const std::string& envMapLocalPath() const { return m_envMapLocalPath; }
    std::string& envMapOverride() { return m_envMapOverride; }
    const std::string& envMapOverride() const { return m_envMapOverride; }
    const std::vector<std::filesystem::path>& envMapMediaList() const { return m_envMapMediaList; }
    std::vector<std::filesystem::path>& envMapMediaList() { return m_envMapMediaList; }

    void refreshEnvironmentMapMediaList(const std::filesystem::path& assetsFolder,
        const std::filesystem::path& currentScenePath);
    void setEnvMapOverrideSource(const std::string& envMapOverride);

    void createOpacityMapsIfSupported(nvrhi::IDevice* device,
        const std::shared_ptr<caustica::DescriptorTableManager>& descriptorTable,
        const std::shared_ptr<caustica::TextureLoader>& textureLoader,
        const std::shared_ptr<caustica::ShaderFactory>& shaderFactory);

    void sceneUnloading();
    void onSceneLoaded(caustica::Scene& scene, PathTracerSettings& settings);
    void resyncLightsFromScene(caustica::Scene& scene);
    void notifySceneReloaded(caustica::Scene& scene);
    int ensureMaterialsFromScene(const std::shared_ptr<caustica::Scene>& scene);

    void applyShaderMacros(std::vector<caustica::ShaderMacro>& macros);
    void createOpacityMicromaps(caustica::Scene& scene);

    void forEachUsedMaterialTexture(
        const std::function<void(caustica::Handle<caustica::ImageAsset>, bool normalMap)>& visitor);

private:
    std::string                                 m_envMapLocalPath;
    std::filesystem::path                       m_envMapMediaFolder;
    std::vector<std::filesystem::path>          m_envMapMediaList;
    std::string                                 m_envMapOverride;

    std::shared_ptr<EnvMapProcessor>            m_environment;
    EnvMapSceneParams                           m_envMapSceneParams;
    std::shared_ptr<LightSamplingCache>         m_lightSampling;
    std::shared_ptr<MaterialGpuCache>           m_materials;
    std::shared_ptr<OpacityMicromapBuilder>     m_opacityMaps;
    std::shared_ptr<ComputePipelineRegistry>    m_computePipelines;

    std::vector<ecs::Entity> m_lightEntities;
};

} // namespace caustica::render

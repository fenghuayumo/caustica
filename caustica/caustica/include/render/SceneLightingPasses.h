#pragma once

#include <render/Passes/Lighting/Distant/EnvMapProcessor.h>
#include <render/Passes/Lighting/LightSamplingCache.h>
#include <render/Passes/Lighting/MaterialGpuCache.h>
#include <render/Passes/OMM/OpacityMicromapBuilder.h>

#include <assets/loader/TextureLoader.h>
#include <render/Core/DescriptorTableManager.h>
#include <render/Core/PathTracerSettings.h>
#include <scene/SceneManager.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ComputePipelineRegistry;
class SceneGraph;

namespace caustica
{
class Light;
class Scene;
class ShaderFactory;
} // namespace caustica

namespace caustica::render
{

// Scene lighting state: GPU cache owners, light list, and environment map selection.
class SceneLightingPasses
{
public:
    std::shared_ptr<EnvMapProcessor>& envMapProcessor() { return m_envMapProcessor; }
    const std::shared_ptr<EnvMapProcessor>& envMapProcessor() const { return m_envMapProcessor; }
    std::shared_ptr<LightSamplingCache>& lightSamplingCache() { return m_lightSamplingCache; }
    const std::shared_ptr<LightSamplingCache>& lightSamplingCache() const { return m_lightSamplingCache; }
    std::shared_ptr<MaterialGpuCache>& materialGpuCache() { return m_materialGpuCache; }
    const std::shared_ptr<MaterialGpuCache>& materialGpuCache() const { return m_materialGpuCache; }
    std::shared_ptr<OpacityMicromapBuilder>& opacityMicromapBuilder() { return m_opacityMicromapBuilder; }
    const std::shared_ptr<OpacityMicromapBuilder>& opacityMicromapBuilder() const { return m_opacityMicromapBuilder; }
    std::shared_ptr<ComputePipelineRegistry>& computePipelineRegistry() { return m_computePipelineRegistry; }
    const std::shared_ptr<ComputePipelineRegistry>& computePipelineRegistry() const { return m_computePipelineRegistry; }

    std::vector<std::shared_ptr<caustica::Light>>& lights() { return m_lights; }
    const std::vector<std::shared_ptr<caustica::Light>>& lights() const { return m_lights; }

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

    void createOpacityMicromapBuilderIfSupported(nvrhi::IDevice* device,
        const std::shared_ptr<caustica::DescriptorTableManager>& descriptorTable,
        const std::shared_ptr<caustica::TextureLoader>& textureLoader,
        const std::shared_ptr<caustica::ShaderFactory>& shaderFactory);

    void sceneUnloading();
    void onSceneLoaded(caustica::Scene& scene, PathTracerSettings& settings);
    void resyncLightsFromSceneGraph(SceneGraph& sceneGraph);
    void notifyGpuCachesSceneReloaded(caustica::Scene& scene);

    void applyGpuCacheShaderMacros(std::vector<caustica::ShaderMacro>& macros);
    void createOpacityMicromaps(caustica::Scene& scene);

    void forEachUsedMaterialTexture(
        const std::function<void(std::shared_ptr<caustica::LoadedTexture>, bool normalMap)>& visitor);

private:
    std::string                                 m_envMapLocalPath;
    std::filesystem::path                       m_envMapMediaFolder;
    std::vector<std::filesystem::path>          m_envMapMediaList;
    std::string                                 m_envMapOverride;

    std::shared_ptr<EnvMapProcessor>                m_envMapProcessor;
    EnvMapSceneParams                           m_envMapSceneParams;
    std::shared_ptr<LightSamplingCache>                m_lightSamplingCache;
    std::shared_ptr<MaterialGpuCache>             m_materialGpuCache;
    std::shared_ptr<OpacityMicromapBuilder>                     m_opacityMicromapBuilder;
    std::shared_ptr<ComputePipelineRegistry>       m_computePipelineRegistry;

    std::vector<std::shared_ptr<caustica::Light>> m_lights;
};

} // namespace caustica::render

#pragma once

#include <render/Passes/Lighting/Distant/EnvMapBaker.h>
#include <render/Passes/Lighting/LightsBaker.h>
#include <render/Passes/Lighting/MaterialsBaker.h>
#include <render/Passes/OMM/OmmBaker.h>

#include <assets/loader/TextureLoader.h>
#include <render/Core/DescriptorTableManager.h>
#include <render/Core/PathTracerSettings.h>
#include <scene/SceneManager.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ComputePipelineBaker;
class SceneGraph;

namespace caustica
{
class Light;
class Scene;
class ShaderFactory;
} // namespace caustica

namespace caustica::editor
{

// Scene lighting state: baker owners, light list, and environment map selection.
class SceneLightingPasses
{
public:
    std::shared_ptr<EnvMapBaker>& envMapBaker() { return m_envMapBaker; }
    const std::shared_ptr<EnvMapBaker>& envMapBaker() const { return m_envMapBaker; }
    std::shared_ptr<LightsBaker>& lightsBaker() { return m_lightsBaker; }
    const std::shared_ptr<LightsBaker>& lightsBaker() const { return m_lightsBaker; }
    std::shared_ptr<MaterialsBaker>& materialsBaker() { return m_materialsBaker; }
    const std::shared_ptr<MaterialsBaker>& materialsBaker() const { return m_materialsBaker; }
    std::shared_ptr<OmmBaker>& ommBaker() { return m_ommBaker; }
    const std::shared_ptr<OmmBaker>& ommBaker() const { return m_ommBaker; }
    std::shared_ptr<ComputePipelineBaker>& computePipelineBaker() { return m_computePipelineBaker; }
    const std::shared_ptr<ComputePipelineBaker>& computePipelineBaker() const { return m_computePipelineBaker; }

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

    void createOmmBakerIfSupported(nvrhi::IDevice* device,
        const std::shared_ptr<caustica::DescriptorTableManager>& descriptorTable,
        const std::shared_ptr<caustica::TextureLoader>& textureLoader,
        const std::shared_ptr<caustica::ShaderFactory>& shaderFactory);

    void sceneUnloading();
    void onSceneLoaded(caustica::Scene& scene, PathTracerSettings& settings);
    void resyncLightsFromSceneGraph(SceneGraph& sceneGraph);
    void notifyBakersSceneReloaded(caustica::Scene& scene);

    void applyBakerShaderMacros(std::vector<caustica::ShaderMacro>& macros);
    void createOpacityMicromaps(caustica::Scene& scene);

    void forEachUsedMaterialTexture(
        const std::function<void(std::shared_ptr<caustica::LoadedTexture>, bool normalMap)>& visitor);

private:
    std::string                                 m_envMapLocalPath;
    std::filesystem::path                       m_envMapMediaFolder;
    std::vector<std::filesystem::path>          m_envMapMediaList;
    std::string                                 m_envMapOverride;

    std::shared_ptr<EnvMapBaker>                m_envMapBaker;
    EnvMapSceneParams                           m_envMapSceneParams;
    std::shared_ptr<LightsBaker>                m_lightsBaker;
    std::shared_ptr<MaterialsBaker>             m_materialsBaker;
    std::shared_ptr<OmmBaker>                     m_ommBaker;
    std::shared_ptr<ComputePipelineBaker>       m_computePipelineBaker;

    std::vector<std::shared_ptr<caustica::Light>> m_lights;
};

} // namespace caustica::editor

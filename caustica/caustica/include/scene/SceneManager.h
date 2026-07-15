#pragma once

#include <scene/SceneLoader.h>

#include <ecs/Entity.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <scene/Scene.h>

namespace caustica
{
class GpuDevice;
class IFileSystem;
class ShaderFactory;
class TextureLoader;
class DescriptorTableManager;
class Scene;
class Material;
class SceneTypeFactory;
class IDescriptorTableManager;
} // namespace caustica

class SceneManager
{
public:
    static constexpr const char* inlineSceneSentinel() { return "__CAUSTICA_INLINE_SCENE_JSON__"; }

    SceneManager(caustica::GpuDevice&                     device,
                 caustica::ShaderFactory&                 shaderFactory,
                 std::shared_ptr<caustica::TextureLoader>  textureCache,
                 std::shared_ptr<caustica::IDescriptorTableManager> descriptorTable,
                 std::shared_ptr<caustica::SceneTypeFactory> sceneTypeFactory = nullptr);

    ~SceneManager();

    void discoverAvailableScenes(const std::filesystem::path& assetsPath);
    const std::vector<std::string>& getAvailableScenes() const { return m_sceneFilesAvailable; }

    [[nodiscard]] const std::shared_ptr<caustica::Scene>& getScene() const { return m_scene; }
    [[nodiscard]] const std::string&                      getCurrentSceneName() const { return m_currentSceneName; }
    [[nodiscard]] const std::filesystem::path&            getCurrentScenePath() const { return m_currentScenePath; }
    [[nodiscard]] const std::string&                      getInlineSceneJson() const { return m_inlineSceneJson; }

    void clearScene();

    bool beginSceneSwitch(const std::string& sceneName,
                          const std::filesystem::path& assetsPath,
                          bool forceReload);

    std::shared_ptr<caustica::Scene> loadScene(std::shared_ptr<caustica::IFileSystem> fs,
                                               const std::filesystem::path&           sceneFileName);

    void setAsyncLoadingEnabled(bool enabled);
    void setLoadingCallbacks(std::function<void()> onLoaded, std::function<void()> onUnloading);
    void beginLoadingScene(std::shared_ptr<caustica::IFileSystem> fs,
                           const std::filesystem::path&           sceneFileName);
    void updateLoading();
    void tickSimulation(uint32_t frameIndex);
    [[nodiscard]] bool isSceneLoading() const;
    [[nodiscard]] bool isSceneLoaded() const;

    // Runtime attach/destroy while a full scene load (or another structure edit) is
    // in flight races Extract → GPU upload → AS rebuild. Gate all ECS structure edits.
    [[nodiscard]] bool tryBeginStructureEdit();
    void endStructureEdit();
    [[nodiscard]] bool isStructureEditInFlight() const { return m_structureEditDepth > 0; }
    [[nodiscard]] bool isSceneStructureBusy() const { return isSceneLoading() || isStructureEditInFlight(); }

    static std::shared_ptr<caustica::Material> findMaterial(
        const std::shared_ptr<caustica::Scene>& scene, int materialID)
    {
        if (!scene) return nullptr;
        for (const auto& mat : scene->getMaterials())
            if (mat->materialID == materialID) return mat;
        return nullptr;
    }

    static caustica::ecs::Entity findEntityByInstanceIndex(
        const std::shared_ptr<caustica::Scene>& scene, int instanceIndex)
    {
        if (!scene || instanceIndex < 0) return caustica::ecs::NullEntity;
        const auto& instances = scene->getMeshInstances();
        if (instanceIndex >= static_cast<int>(instances.size())) return caustica::ecs::NullEntity;
        return instances[instanceIndex];
    }

    struct ResolvedScenePath
    {
        std::filesystem::path path;
        bool                  inlineScene = false;
        std::string           inlineJson;
    };

    static ResolvedScenePath resolveScenePath(
        const std::string&              sceneName,
        const std::filesystem::path&    assetsPath);

    static void onSceneLoadedGpuPrep(caustica::Scene& scene, bool& accelRebuildRequested);

    static void refreshEnvironmentMapMediaList(
        const std::filesystem::path&              assetsPath,
        const std::filesystem::path&              envMapSubFolder,
        const std::filesystem::path&              currentScenePath,
        std::vector<std::filesystem::path>&       outMediaList,
        std::filesystem::path&                    outMediaFolder);

    caustica::GpuDevice&                      getDevice()        { return m_device; }
    caustica::ShaderFactory&                  getShaderFactory()  { return m_shaderFactory; }
    std::shared_ptr<caustica::TextureLoader>   getTextureLoader()   { return m_textureCache; }
    std::shared_ptr<caustica::IDescriptorTableManager> getDescriptorTable() { return m_descriptorTable; }

private:
    caustica::GpuDevice&                      m_device;
    caustica::ShaderFactory&                  m_shaderFactory;
    std::shared_ptr<caustica::TextureLoader>   m_textureCache;
    std::shared_ptr<caustica::IDescriptorTableManager> m_descriptorTable;
    std::shared_ptr<caustica::SceneTypeFactory>       m_sceneTypeFactory;

    std::vector<std::string>                  m_sceneFilesAvailable;
    std::shared_ptr<caustica::Scene>          m_scene;
    std::string                               m_currentSceneName;
    std::filesystem::path                     m_currentScenePath;
    std::string                               m_inlineSceneJson;

    caustica::SceneLoader                     m_loader;
    int                                       m_structureEditDepth = 0;
};

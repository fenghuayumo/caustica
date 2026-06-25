#pragma once

#include <scene/SceneLoader.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <scene/Scene.h>
#include <scene/SceneGraph.h>

namespace caustica
{
class GpuDevice;
class IFileSystem;
class ShaderFactory;
class TextureLoader;
class DescriptorTableManager;
class Scene;
class SceneGraphNode;
class Material;
} // namespace caustica

// =============================================================================
// SceneManager — Engine layer: scene discovery, loading, queries, env map listing.
// =============================================================================
class SceneManager
{
public:
    static constexpr const char* inlineSceneSentinel() { return "__CAUSTICA_INLINE_SCENE_JSON__"; }

    SceneManager(caustica::GpuDevice&                     device,
                 caustica::ShaderFactory&                 shaderFactory,
                 std::shared_ptr<caustica::TextureLoader>  textureCache,
                 std::shared_ptr<caustica::DescriptorTableManager> descriptorTable);

    ~SceneManager();

    // --- Scene discovery ---
    void discoverAvailableScenes(const std::filesystem::path& assetsPath);
    const std::vector<std::string>& getAvailableScenes() const { return m_sceneFilesAvailable; }

    // --- Active scene state ---
    [[nodiscard]] const std::shared_ptr<caustica::Scene>& getScene() const { return m_scene; }
    [[nodiscard]] const std::string&                      getCurrentSceneName() const { return m_currentSceneName; }
    [[nodiscard]] const std::filesystem::path&            getCurrentScenePath() const { return m_currentScenePath; }
    [[nodiscard]] const std::string&                      getInlineSceneJson() const { return m_inlineSceneJson; }

    void clearScene();

    // Resolve path + update active-scene metadata.  Returns false when the switch is a no-op.
    bool beginSceneSwitch(const std::string& sceneName,
                          const std::filesystem::path& assetsPath,
                          bool forceReload);

    // Parse and load scene content from disk or inline JSON.  Returns nullptr on failure.
    std::shared_ptr<caustica::Scene> loadScene(std::shared_ptr<caustica::IFileSystem> fs,
                                               const std::filesystem::path&           sceneFileName);

    // --- Async/sync loading orchestration (SceneLoader) ---
    void setAsyncLoadingEnabled(bool enabled);
    void setLoadingCallbacks(std::function<void()> onLoaded, std::function<void()> onUnloading);
    void beginLoadingScene(std::shared_ptr<caustica::IFileSystem> fs,
                           const std::filesystem::path&           sceneFileName);
    void updateLoading();
    [[nodiscard]] bool isSceneLoading() const;
    [[nodiscard]] bool isSceneLoaded() const;

    // --- Scene queries (static, take scene as parameter) ---
    static std::shared_ptr<caustica::Material> findMaterial(
        const std::shared_ptr<caustica::Scene>& scene, int materialID)
    {
        if (!scene) return nullptr;
        for (const auto& mat : scene->GetSceneGraph()->GetMaterials())
            if (mat->materialID == materialID) return mat;
        return nullptr;
    }

    static std::shared_ptr<caustica::SceneGraphNode> findNodeByInstanceIndex(
        const std::shared_ptr<caustica::Scene>& scene, int instanceIndex)
    {
        if (!scene || instanceIndex < 0) return nullptr;
        const auto& instances = scene->GetSceneGraph()->GetMeshInstances();
        if (instanceIndex >= static_cast<int>(instances.size())) return nullptr;
        return instances[instanceIndex]->GetNodeSharedPtr();
    }

    // --- Scene path resolution (does not load) ---
    struct ResolvedScenePath
    {
        std::filesystem::path path;
        bool                  inlineScene = false;
        std::string           inlineJson;
    };

    static ResolvedScenePath resolveScenePath(
        const std::string&              sceneName,
        const std::filesystem::path&    assetsPath);

    // GPU-side prep after a scene finishes loading (accel dirty, animation t=0).
    static void onSceneLoadedGpuPrep(caustica::Scene& scene, bool& accelRebuildRequested);

    // --- Environment map listing ---
    static void refreshEnvironmentMapMediaList(
        const std::filesystem::path&              assetsPath,
        const std::filesystem::path&              envMapSubFolder,
        const std::filesystem::path&              currentScenePath,
        std::vector<std::filesystem::path>&       outMediaList,
        std::filesystem::path&                    outMediaFolder);

    // --- Dependencies ---
    caustica::GpuDevice&                      getDevice()        { return m_device; }
    caustica::ShaderFactory&                  getShaderFactory()  { return m_shaderFactory; }
    std::shared_ptr<caustica::TextureLoader>   getTextureLoader()   { return m_textureCache; }
    std::shared_ptr<caustica::DescriptorTableManager> getDescriptorTable() { return m_descriptorTable; }

private:
    caustica::GpuDevice&                      m_device;
    caustica::ShaderFactory&                  m_shaderFactory;
    std::shared_ptr<caustica::TextureLoader>   m_textureCache;
    std::shared_ptr<caustica::DescriptorTableManager> m_descriptorTable;

    std::vector<std::string>                  m_sceneFilesAvailable;
    std::shared_ptr<caustica::Scene>          m_scene;
    std::string                               m_currentSceneName;
    std::filesystem::path                     m_currentScenePath;
    std::string                               m_inlineSceneJson;

    caustica::SceneLoader                     m_loader;
};

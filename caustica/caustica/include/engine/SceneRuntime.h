#pragma once

#include <backend/GpuDevice.h>
#include <core/command_line.h>
#include <core/progress.h>
#include <math/math.h>
#include <render/Core/CameraController.h>
#include <render/Core/PathTracerSettings.h>
#include <render/Core/SceneCameraController.h>
#include <render/Core/TextureUtils.h>
#include <render/RenderRuntimeState.h>
#include <render/RenderSessionState.h>
#include <render/SessionDiagnostics.h>
#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <scene/SceneManager.h>
#include <ecs/Entity.h>
#include <scene/camera/Camera.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <rhi/nvrhi.h>

class RenderTargets;
struct DebugFeedbackStruct;
struct DeltaTreeVizPathVertex;

namespace caustica
{
class Application;
class BindingCache;
class CommonRenderPasses;
class DescriptorTableManager;
class GpuRenderSubsystem;
class FirstPersonCamera;
class Material;
class MeshInfo;
class PlanarView;
class ShaderFactory;
class TextureLoader;
} // namespace caustica

namespace caustica::render
{
class WorldRenderer;
class SceneGaussianSplatPasses;
class SceneLightingPasses;
class SceneRayTracingResources;
} // namespace caustica::render

namespace caustica
{

// Runtime scene session: loading, camera, animation, path-tracer frame driving.
// Editor-only features live in caustica::editor::SceneEditor (derived class).
class SceneRuntime
{
public:
    SceneRuntime(const CommandLineOptions& cmdLine,
        render::RenderSessionState& sessionState,
        render::SessionDiagnostics& diagnostics);
    virtual ~SceneRuntime();

    void setGpuDevice(GpuDevice& gpuDevice) { m_gpuDevice = &gpuDevice; }
    void setApplication(Application* application) { m_application = application; }

    std::shared_ptr<ShaderFactory> GetShaderFactory() const { return m_shaderFactory; }
    std::shared_ptr<CommonRenderPasses> GetCommonPasses() const;
    std::shared_ptr<TextureLoader> GetTextureLoader() const { return m_TextureLoader; }
    std::shared_ptr<DescriptorTableManager> GetDescriptorTable() const;
    SceneManager* GetSceneManager() const { return m_sceneManager; }
    CameraController* GetCameraController() const { return m_camera; }
    nvrhi::ITexture* GetLdrColorTexture() const;
    std::shared_ptr<Scene> GetScene() const;
    std::vector<std::string> const& GetAvailableScenes() const;
    std::string GetCurrentSceneName() const;
    const DebugFeedbackStruct& GetFeedbackData() const;
    const DeltaTreeVizPathVertex* GetDebugDeltaPathTree() const;
    uint GetSceneCameraCount() const;
    uint& SelectedCameraIndex();

    render::RenderSessionState& GetRenderSessionState() { return m_sessionState; }
    const render::RenderSessionState& GetRenderSessionState() const { return m_sessionState; }
    PathTracerSettings& GetPathTracerSettings() { return m_settings; }
    const PathTracerSettings& GetPathTracerSettings() const { return m_settings; }
    render::RenderRuntimeState& GetRenderRuntimeState() { return m_renderState; }
    const render::RenderRuntimeState& GetRenderRuntimeState() const { return m_renderState; }

    std::shared_ptr<Material> FindMaterial(int materialID) const;
    ecs::Entity FindEntityByInstanceIndex(int instanceIndex) const;

    void CollectUncompressedTextures();
    auto& GetUncompressedTextures() { return m_uncompressedTextures; }

    void SaveCurrentCamera() const;
    void LoadCurrentCamera();
    std::string GetCurrentCameraPosDirUp() const;
    bool SetCurrentCameraPosDirUp(const std::string& val);

    float GetCameraVerticalFOV() const;
    void SetCameraVerticalFOV(float cameraFOV);
    void SetCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height);
    void ClearCameraIntrinsics();

    float GetAvgTimePerFrame() const;

    void bindGpuRenderSubsystem(GpuRenderSubsystem& gpuRenderSubsystem);
    void Init(const std::string& preferredScene,
        const std::shared_ptr<ShaderFactory>& shaderFactory);

    [[nodiscard]] GpuRenderSubsystem* GetGpuRenderSubsystem() const { return m_gpuRenderSubsystem; }

    render::SceneLightingPasses& GetLightingPasses();
    const render::SceneLightingPasses& GetLightingPasses() const;
    render::SceneRayTracingResources& GetRayTracingResources();
    const render::SceneRayTracingResources& GetRayTracingResources() const;
    render::SceneGaussianSplatPasses& GetGaussianSplatPasses();
    const render::SceneGaussianSplatPasses& GetGaussianSplatPasses() const;
    [[nodiscard]] render::WorldRenderer* GetWorldRenderer() const;

    void initStreamlineAndWindow();

    void prepareRenderFrame();
    virtual void afterWorldRender(GpuDevice& gpuDevice);
    void recordFrameTiming(const GpuDevice& gpuDevice);
    void beginFrame();
    bool processPendingSceneSwitch();
    [[nodiscard]] bool shouldSkipRender() const;
    void SetCurrentScene(const std::string& sceneName, bool forceReload = false);
    bool IsSceneLoading() const;
    bool IsSceneLoaded() const;
    bool LoadGaussianSplatFile(const std::filesystem::path& fileName, bool convertRdfToRub = true);
    uint32_t GetGaussianSplatCount() const;
    uint32_t GetGaussianSplatObjectCount() const;
    const std::string& GetGaussianSplatFileName() const;

    virtual void SceneUnloading();
    virtual void SceneLoaded();
    virtual bool ShouldRenderUnfocused() const;
    virtual void Animate(float elapsedTimeSeconds);

    void RequestMeshAccelRebuild(const std::shared_ptr<MeshInfo>& mesh);

    void BackBufferResizing();

    std::string GetResolutionInfo() const;
    std::string GetFPSInfo() const { return m_fpsInfo; }

    void DebugDrawLine(math::float3 start, math::float3 stop, math::float4 col1, math::float4 col2);
    const FirstPersonCamera& GetCurrentCamera() const;
    const std::shared_ptr<PlanarView>& GetCurrentView() const;

    void SetSceneTime(double sceneTime);
    double GetSceneTime();

    bool IsEnvMapLoaded() const { return true; }
    const std::string& GetEnvMapLocalPath() const;
    const std::string& GetEnvMapOverrideSource() const;
    void SetEnvMapOverrideSource(const std::string& envMapOverride);
    const std::vector<std::filesystem::path>& GetEnvMapMediaList();

    double& GetSceneTimeRef() { return m_sceneTime; }
    BindingCache& GetBindingCache();

    [[nodiscard]] GpuDevice& GetGpuDevice() const { return *m_gpuDevice; }
    [[nodiscard]] nvrhi::IDevice* GetDevice() const { return m_gpuDevice->GetDevice(); }
    [[nodiscard]] uint32_t GetFrameIndex() const { return m_gpuDevice->GetFrameIndex(); }

    GLFWwindow* GetGLFWWindow() const { return m_gpuDevice->GetWindow(); }

    int GetAccumulationSampleIndex() const;
    math::uint2 GetRenderSize() const;
    math::uint2 GetDisplaySize() const;

    bool HasAsyncLoadingInProgress() const
    {
        return m_sessionDiagnostics.asyncLoadingInProgress
            || m_renderState.Invalidation.ShaderAndACRefreshDelayedRequest > 0;
    }

    bool AccumulationCompleted() const;
    const PlanarView& GetView() const;

protected:
    virtual void onBeforeInitialSceneLoad() {}

    void applySceneSwitch(const std::string& sceneName, bool forceReload);
    void tickSceneSwitchTest();
    void syncCameraFromScene();
    void runGpuWorkOnRenderThread(const std::function<void()>& work);
    void UpdateFpsInfo(double frameTimeSeconds);
    virtual void updateWindowTitle();

    render::RenderSessionState& m_sessionState;
    PathTracerSettings& m_settings;
    render::RenderRuntimeState& m_renderState;
    render::SessionDiagnostics& m_sessionDiagnostics;

    Application* m_application = nullptr;
    GpuRenderSubsystem* m_gpuRenderSubsystem = nullptr;

    SceneCameraController m_cameraController;

    GpuDevice* m_gpuDevice = nullptr;

    SceneManager* m_sceneManager = nullptr;
    CameraController* m_camera = nullptr;

    double m_sceneTime = 0.;
    float m_lastDeltaTime = 0.0f;

    std::shared_ptr<ShaderFactory> m_shaderFactory;
    std::shared_ptr<TextureLoader> m_TextureLoader;
    std::shared_ptr<CommonRenderPasses> m_CommonPasses;
    BindingCache* m_bindingCache = nullptr;
    std::shared_ptr<DescriptorTableManager> m_DescriptorTable;

    render::SceneLightingPasses* m_lightingPasses = nullptr;
    render::SceneRayTracingResources* m_rayTracingResources = nullptr;
    render::SceneGaussianSplatPasses* m_gaussianSplatPasses = nullptr;

    std::map<std::shared_ptr<LoadedTexture>, TextureCompressionType> m_uncompressedTextures;

    std::string m_fpsInfo;
    bool m_windowIsInFocus = true;

    ProgressBar m_progressLoading;

    std::mutex m_pendingSceneSwitchMutex;
    struct PendingSceneSwitch
    {
        std::string sceneName;
        bool forceReload = false;
    };
    std::optional<PendingSceneSwitch> m_pendingSceneSwitch;

    int m_sceneSwitchTestFramesUntilSwitch = 0;
    size_t m_sceneSwitchTestSceneIndex = 0;
    int m_sceneSwitchTestSwitchesDone = 0;

    const CommandLineOptions& m_cmdLine;
};

} // namespace caustica

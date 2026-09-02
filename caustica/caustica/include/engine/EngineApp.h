#pragma once

// Public embedding entry. Prefer #include <caustica.h> in new apps.
// Contract: docs/public-api.md

#include <backend/GpuDevice.h>
#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/EntryPoint.h>
#include <engine/EngineSceneCallbacks.h>
#include <engine/SceneQuery.h>
#include <engine/SceneSpawn.h>
#include <engine/SceneTransform.h>
#include <engine/SystemSets.h>
#include <engine/EnqueueRenderCommand.h>
#include <engine/SceneLifecycle.h>
#include <engine/CameraApi.h>
#include <engine/MeshDeformApi.h>
#include <engine/RenderSessionApi.h>
#include <engine/SceneViewState.h>
#include <core/command_line.h>
#include <math/math.h>
#include <render/RenderAppState.h>
#include <render/AppDiagnostics.h>
#include <render/core/PathTracerSettings.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace caustica
{

// Single create config for EngineApp. Parse argv with fromArgv(); do not keep a
// second live copy of these fields on the host.
struct EngineAppDesc
{
    uint32_t width = 1920;
    uint32_t height = 1080;
    bool headless = false;
    bool dedicatedRenderThread = true;
    // Bevy-style concurrent system execution (ADR 0003). Turn off to make a
    // schedule run one system at a time while debugging.
    bool parallelSystems = true;
    bool debugDevice = false;
    caustica::rhi::AdapterSelector adapter;
    bool useVulkan = false;
    bool fullscreen = false;
    bool maximized = false;
    std::string scene = "default.scene.json";
    std::string windowTitle = "caustica";

    // Empty = auto-discover next to the executable / module (ShaderBin, Assets).
    std::filesystem::path runtimeDirectory;
    // Directory that contains the Assets/ pack folder. Empty = discover.
    std::filesystem::path resourceRoot;
    // Direct path to the asset pack (Assets/ or CAUSTICA_ASSETS_DIR). Empty = discover.
    std::filesystem::path assetPackRoot;

#if CAUSTICA_WITH_DX12
    ID3D12DeviceFactory* d3d12DeviceFactory = nullptr;
#endif

    // Python Device-outlives-App only. All three must be set together.
    GpuDevice* device = nullptr;
    Window* window = nullptr;
    GpuSurface* surface = nullptr;

    // Snapshot of parsed CLI (capture / path-tracer overrides / console). EngineApp owns a copy.
    CommandLineOptions cli{};

    bool hasSceneCallbacks = false;
    EngineSceneCallbacks sceneCallbacks{};

    // Called before GpuDevice::create (e.g. stop splash).
    AppHook preGpuDeviceInit = nullptr;

    [[nodiscard]] static std::optional<EngineAppDesc> fromArgv(int argc, char const* const* argv);
    bool applyCommandLine(const CommandLineOptions& options);
};

// Expand `builtin:plane_cube` (and similar) into inline scene JSON. Other
// values pass through unchanged. EngineApp::create / setScene call this.
[[nodiscard]] std::string prepareSceneSource(const std::string& scene);
[[nodiscard]] std::string builtinSceneJson(const std::string& builtinModel = "plane_cube");

struct LdrFramebuffer
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 4;
    std::vector<uint8_t> pixels;
};

// Bevy-style embed entry: create → add systems/plugins → run() / stepFrame().
//
//   auto engine = caustica::EngineApp::create({ .scene = "Kitchen/kitchen.json" });
//   engine->addSystem<MySimLabel>(AppSchedule::update,
//       [](EntityWorld scene, ecs::Query<scene::LocalTransformComponent> q) { ... });
//   engine->run(); // finishStartup runs automatically
//
// Headless:
//   auto engine = caustica::EngineApp::create({ .headless = true });
//   while (running) engine->stepFrame(); // also auto-starts
class EngineApp
{
public:
    [[nodiscard]] static std::unique_ptr<EngineApp> create(EngineAppDesc desc);

    ~EngineApp();

    EngineApp(const EngineApp&) = delete;
    EngineApp& operator=(const EngineApp&) = delete;

    [[nodiscard]] bool isValid() const { return m_valid; }

    bool finishStartup();

    void run();
    bool stepFrame(float dtSeconds = -1.f);
    void requestExit();
    void shutdown();

    template<typename Label, class F>
    EngineApp& addSystem(AppSchedule schedule, F&& system, AppSystemOrdering ordering = {})
    {
        m_app->addSystem<Label>(schedule, std::forward<F>(system), std::move(ordering));
        return *this;
    }

    template<typename T, typename... Args>
    T& emplaceResource(Args&&... args)
    {
        return m_app->emplaceResource<T>(std::forward<Args>(args)...);
    }

    template<typename T, typename... Args>
    EngineApp& addPlugin(Args&&... args)
    {
        m_app->addPlugin<T>(std::forward<Args>(args)...);
        return *this;
    }

    // Owned schedule runtime. Editor and engine plugins; hosts use EngineApp.
    [[nodiscard]] App& app();
    [[nodiscard]] const App& app() const;
    [[nodiscard]] GpuDevice* device() const;
    [[nodiscard]] GpuSurface* surface() const;
    [[nodiscard]] Window* window() const;

    [[nodiscard]] SceneViewState& viewState() { return m_viewState; }
    [[nodiscard]] const SceneViewState& viewState() const { return m_viewState; }
    [[nodiscard]] render::AppDiagnostics& diagnostics() { return m_diagnostics; }
    [[nodiscard]] const render::AppDiagnostics& diagnostics() const { return m_diagnostics; }

    void setScene(const std::string& name, bool forceReload = false);
    [[nodiscard]] bool isSceneLoaded() const;
    [[nodiscard]] bool isSceneLoading() const;
    [[nodiscard]] bool isSceneReady() const;
    bool waitUntilReady(double timeoutSeconds = 600.0, int warmupFrames = 4);
    [[nodiscard]] std::string currentSceneName() const;
    [[nodiscard]] const std::vector<std::string>& availableScenes() const;
    [[nodiscard]] scene::SceneEntityWorld* entityWorld() const;
    [[nodiscard]] PathTracerSettings& settings();
    [[nodiscard]] const PathTracerSettings& settings() const;
    [[nodiscard]] render::RenderAppState& renderAppState();
    [[nodiscard]] const render::RenderAppState& renderAppState() const;
    [[nodiscard]] CommandLineOptions& commandLine();
    [[nodiscard]] const CommandLineOptions& commandLine() const;

    bool setCameraPosDirUp(const math::float3& pos, const math::float3& dir, const math::float3& up);
    bool setCameraPosDirUp(const std::string& value);
    [[nodiscard]] std::string currentCameraPosDirUp() const;
    [[nodiscard]] CameraPose currentCameraPose() const;
    bool setCameraPose(const CameraPose& pose);
    bool setCameraVerticalFOV(float radians);
    [[nodiscard]] float cameraVerticalFOV() const;
    bool setCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height);
    bool clearCameraIntrinsics();
    [[nodiscard]] uint32_t sceneCameraCount() const;
    [[nodiscard]] uint32_t selectedCameraIndex() const;
    bool setSelectedCameraIndex(uint32_t index);
    [[nodiscard]] ecs::Entity activeCameraEntity() const;
    [[nodiscard]] bool activeCameraIsFree() const;
    [[nodiscard]] std::string activeCameraPath() const;
    [[nodiscard]] std::string activeCameraName() const;
    bool setActiveCamera(ecs::Entity entity);
    bool setActiveCameraByPath(const std::string& path);
    void saveCurrentCamera();
    void loadCurrentCamera();

    Handle<ScenePrefabAsset> load(const std::filesystem::path& path);
    ecs::Entity spawn(
        const Handle<ScenePrefabAsset>& prefab,
        const SceneApplyCallbacks& callbacks = {});
    ecs::Entity spawnFromFile(
        const std::filesystem::path& path,
        const SceneApplyCallbacks& callbacks = {});
    ecs::Entity spawnFromSource(
        const std::string& source,
        const SceneApplyCallbacks& callbacks = {});
    bool despawn(ecs::Entity entity);
    ecs::Entity spawnDirectionalLight(
        scene::DirectionalLightComponent component, const std::string& name = {});
    ecs::Entity spawnSpotLight(
        scene::SpotLightComponent component, const std::string& name = {});
    ecs::Entity spawnPointLight(
        scene::PointLightComponent component, const std::string& name = {});
    ecs::Entity spawnRectLight(
        scene::RectLightComponent component, const std::string& name = {});
    ecs::Entity spawnEnvironmentLight(
        scene::EnvironmentLightComponent component, const std::string& name = {});

    bool loadGaussianSplatFile(
        const std::filesystem::path& fileName, bool convertRdfToRub = true);
    [[nodiscard]] uint32_t gaussianSplatCount() const;
    [[nodiscard]] uint32_t gaussianSplatObjectCount() const;
    [[nodiscard]] const std::string& gaussianSplatFileName() const;

    [[nodiscard]] ecs::Entity findEntity(
        const std::filesystem::path& path, ecs::Entity context = ecs::NullEntity) const;
    [[nodiscard]] std::shared_ptr<Material> findMaterial(int materialID) const;

    [[nodiscard]] std::vector<dm::float3> getMeshVertices(ecs::Entity entity);
    [[nodiscard]] std::vector<dm::float3> getMeshVerticesWorld(ecs::Entity entity);
    void setMeshVertices(
        ecs::Entity entity,
        const std::vector<dm::float3>& vertices,
        const MeshDeformOptions& options = {});
    void setMeshVerticesWorld(
        ecs::Entity entity,
        const std::vector<dm::float3>& vertices,
        const MeshDeformOptions& options = {});
    void requestMeshAccelRebuild(ecs::Entity entity, bool resetAccumulation = true);
    void requestFullAccelRebuild();
    uint32_t precacheRtFeaturePresets(bool showProgress = true);

    void setEnvMapOverrideSource(const std::string& path);
    [[nodiscard]] double sceneTime() const;
    void setSceneTime(double seconds);

    void setRealtimeMode(bool standaloneDenoiser = true, int realtimeAA = 2);
    void setReferenceMode(
        int spp = 0,
        bool oidn = false,
        int oidnQuality = 1,
        int oidnPasses = 1,
        int oidnPrefilter = 1);
    bool prepareAnimationFrame(
        double sceneTime, bool importedAnimations = true, bool keyframes = true);

    [[nodiscard]] bool accumulationCompleted() const;
    [[nodiscard]] int accumulationSampleIndex() const;
    [[nodiscard]] math::uint2 renderSize() const;
    [[nodiscard]] float avgTimePerFrame() const;
    [[nodiscard]] std::string fpsInfo() const;
    [[nodiscard]] std::string resolutionInfo() const;
    [[nodiscard]] caustica::rhi::Texture* ldrColorTexture() const;
    [[nodiscard]] uint32_t frameIndex() const;
    bool saveScreenshot(const std::filesystem::path& path);
    [[nodiscard]] std::optional<LdrFramebuffer> readLdrFramebuffer();

private:
    EngineApp() = default;

    bool initialize(EngineAppDesc desc);

    EngineAppDesc m_desc{};
    CommandLineOptions m_cmdLine{};
    render::RenderAppState m_renderAppState{};
    render::AppDiagnostics m_diagnostics{};
    SceneViewState m_viewState{};

    std::unique_ptr<GpuDevice> m_ownedDevice;
    std::unique_ptr<Window> m_ownedWindow;
    std::unique_ptr<GpuSurface> m_ownedSurface;
    GpuDevice* m_device = nullptr;
    Window* m_window = nullptr;
    GpuSurface* m_surface = nullptr;

    std::unique_ptr<App> m_app;

    bool m_valid = false;
    bool m_ownsDevice = false;
};

} // namespace caustica

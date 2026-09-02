#pragma once

// Public embedding entry. Prefer #include <caustica.h> in new apps.
// Contract: docs/public-api.md — do not assemble DefaultPlugins or dig WorldRenderer.

#include <backend/GpuDevice.h>
#include <engine/App.h>
#include <engine/EntryPoint.h>
#include <engine/EngineSceneCallbacks.h>
#include <engine/SceneQuery.h>
#include <engine/SceneSpawn.h>
#include <engine/SceneTransform.h>
#include <engine/SystemSets.h>
#include <engine/EnqueueRenderCommand.h>
#include <engine/SceneLifecycle.h>
#include <engine/CameraApi.h>
#include <engine/RenderSessionApi.h>
#include <engine/SceneViewState.h>
#include <core/command_line.h>
#include <render/RenderAppState.h>
#include <render/AppDiagnostics.h>
#include <render/core/PathTracerSettings.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

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
    [[nodiscard]] scene::SceneEntityWorld* entityWorld() const;
    [[nodiscard]] PathTracerSettings& settings();
    [[nodiscard]] const PathTracerSettings& settings() const;
    [[nodiscard]] render::RenderAppState& renderAppState();
    [[nodiscard]] const render::RenderAppState& renderAppState() const;
    [[nodiscard]] CommandLineOptions& commandLine();
    [[nodiscard]] const CommandLineOptions& commandLine() const;

    bool setCameraPosDirUp(const std::string& value);
    void setCameraVerticalFOV(float radians);
    void setCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height);
    [[nodiscard]] bool accumulationCompleted() const;
    [[nodiscard]] caustica::rhi::Texture* ldrColorTexture() const;
    [[nodiscard]] uint32_t frameIndex() const;

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

#pragma once

#include <backend/GpuDevice.h>
#include <engine/App.h>
#include <engine/EntryPoint.h>
#include <engine/EngineSceneCallbacks.h>
#include <engine/SceneQuery.h>
#include <engine/SceneSpawn.h>
#include <engine/SceneLifecycle.h>
#include <engine/CameraApi.h>
#include <engine/RenderSessionApi.h>
#include <engine/SceneViewState.h>
#include <core/command_line.h>
#include <render/RenderAppState.h>
#include <render/AppDiagnostics.h>
#include <render/core/PathTracerSettings.h>
#include <scene/Scene.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace caustica
{

// Minimal config for embedding caustica in a new application.
// Prefer EngineApp::create() over assembling SceneAppConfig / DefaultPlugins yourself.
struct EngineAppDesc
{
    uint32_t width = 1920;
    uint32_t height = 1080;
    bool headless = false;
    bool dedicatedRenderThread = true;
    bool debugDevice = false;
    int adapterIndex = -1;
    bool useVulkan = false;
    bool fullscreen = false;
    std::string scene = "default.json";
    std::string windowTitle = "caustica";

    // Empty = auto-discover next to the executable / module (ShaderPrecompiled, Assets).
    std::filesystem::path runtimeDirectory;
    std::filesystem::path resourceRoot;

#if CAUSTICA_WITH_DX12
    ID3D12DeviceFactory* d3d12DeviceFactory = nullptr;
#endif

    // Optional: inject an already-created device/window. EngineApp does not take ownership.
    GpuDevice* device = nullptr;
    Window* window = nullptr;

    // When false, caller adds more plugins then calls EngineApp::finishStartup().
    bool finishStartup = true;

    // Optional host-owned state (nullptr = EngineApp owns internals as today).
    SceneViewState* viewState = nullptr;
    render::AppDiagnostics* diagnostics = nullptr;
    render::RenderAppState* renderState = nullptr;
    CommandLineOptions* cmdLine = nullptr;

    bool applyCmdLineToRenderState = true;
    bool hasSceneCallbacks = false;
    EngineSceneCallbacks sceneCallbacks{};

    // Called before GpuDevice::createInitialized (e.g. stop splash).
    AppHook preGpuDeviceInit = nullptr;
};

// Bevy-style embed entry: one create() call, then run() or stepFrame().
//
//   auto engine = caustica::EngineApp::create({ .scene = "Kitchen/kitchen.json" });
//   engine->app().addSystem<MySimLabel>(AppSchedule::update, [](SystemContext& ctx) {
//       if (auto* ew = ctx.entityWorld())
//           ew->world().each<scene::LocalTransformComponent>(...);
//   });
//   engine->run();
//
// Headless:
//   auto engine = caustica::EngineApp::create({ .headless = true });
//   while (running) engine->stepFrame();
//
// Scene mutations: caustica::load/spawn/despawn (ECS only; Extract flushes GPU).
// Scene queries: ctx.entityWorld() / caustica::entityWorld(app) -- not GpuRenderSubsystem.
class EngineApp
{
public:
    [[nodiscard]] static std::unique_ptr<EngineApp> create(EngineAppDesc desc);

    ~EngineApp();

    EngineApp(const EngineApp&) = delete;
    EngineApp& operator=(const EngineApp&) = delete;

    [[nodiscard]] bool isValid() const { return m_valid; }

    // Call when create used finishStartup=false (after adding host plugins).
    bool finishStartup();

    void run();
    bool stepFrame(float dtSeconds = -1.f);
    void requestExit();
    void shutdown();

    [[nodiscard]] App& app();
    [[nodiscard]] const App& app() const;
    [[nodiscard]] GpuDevice* device() const;

    void setScene(const std::string& name, bool forceReload = false);
    [[nodiscard]] std::shared_ptr<Scene> scene() const;
    [[nodiscard]] bool isSceneLoaded() const;
    [[nodiscard]] bool isSceneLoading() const;
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

    CommandLineOptions* m_cmdLinePtr = &m_cmdLine;
    render::RenderAppState* m_renderStatePtr = &m_renderAppState;
    render::AppDiagnostics* m_diagnosticsPtr = &m_diagnostics;
    SceneViewState* m_viewStatePtr = &m_viewState;

    std::unique_ptr<GpuDevice> m_ownedDevice;
    std::unique_ptr<Window> m_ownedWindow;
    GpuDevice* m_device = nullptr;
    Window* m_window = nullptr;

    std::unique_ptr<App> m_app;

    bool m_valid = false;
    bool m_ownsDevice = false;
};

} // namespace caustica

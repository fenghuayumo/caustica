#pragma once

#include <backend/GpuDevice.h>
#include <engine/App.h>
#include <engine/SceneSessionSystems.h>
#include <engine/SceneViewState.h>
#include <core/command_line.h>
#include <render/RenderSessionState.h>
#include <render/SessionDiagnostics.h>
#include <render/core/PathTracerSettings.h>
#include <scene/Scene.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace caustica
{

// Minimal config for embedding caustica in a new application.
// Prefer EngineApp::create() over assembling SceneSessionConfig / DefaultPlugins yourself.
struct EngineAppDesc
{
    uint32_t width = 1920;
    uint32_t height = 1080;
    bool headless = false;
    bool dedicatedRenderThread = true;
    bool debugDevice = false;
    int adapterIndex = -1;
    bool useVulkan = false;
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
};

// Bevy-style embed entry: one create() call, then run() or stepFrame().
//
//   auto engine = caustica::EngineApp::create({ .scene = "Kitchen/kitchen.json" });
//   engine->app().addSystem(AppSchedule::update, "MySim", [](SystemContext& ctx) { ... });
//   engine->run();
//
// Headless:
//   auto engine = caustica::EngineApp::create({ .headless = true });
//   while (running) engine->stepFrame();
//
// Scene / settings / camera: use sceneSession::load/spawn/despawn and free functions
// on engine->app(), or the convenience methods below.
class EngineApp
{
public:
    [[nodiscard]] static std::unique_ptr<EngineApp> create(EngineAppDesc desc);

    ~EngineApp();

    EngineApp(const EngineApp&) = delete;
    EngineApp& operator=(const EngineApp&) = delete;

    [[nodiscard]] bool isValid() const { return m_valid; }

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
    [[nodiscard]] render::RenderSessionState& renderSessionState();
    [[nodiscard]] const render::RenderSessionState& renderSessionState() const;
    [[nodiscard]] CommandLineOptions& commandLine();
    [[nodiscard]] const CommandLineOptions& commandLine() const;

    bool setCameraPosDirUp(const std::string& value);
    void setCameraVerticalFOV(float radians);
    void setCameraIntrinsics(float fx, float fy, float cx, float cy, float width, float height);
    [[nodiscard]] bool accumulationCompleted() const;
    [[nodiscard]] nvrhi::ITexture* ldrColorTexture() const;
    [[nodiscard]] uint32_t frameIndex() const;

private:
    EngineApp() = default;

    bool initialize(EngineAppDesc desc);

    EngineAppDesc m_desc{};
    CommandLineOptions m_cmdLine{};
    render::RenderSessionState m_sessionState{};
    render::SessionDiagnostics m_diagnostics{};
    SceneViewState m_viewState{};

    std::unique_ptr<GpuDevice> m_ownedDevice;
    std::unique_ptr<Window> m_ownedWindow;
    GpuDevice* m_device = nullptr;
    Window* m_window = nullptr;

    std::unique_ptr<App> m_app;

    bool m_valid = false;
    bool m_ownsDevice = false;
};

} // namespace caustica

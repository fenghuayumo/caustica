#include <engine/SceneSessionStartup.h>

#include <scene/Scene.h>

#include <assets/AssetSystem.h>
#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/SceneSessionSystems.h>

#include <core/path_utils.h>
#include <render/core/RenderSceneTypeFactory.h>
#include <render/RenderSessionState.h>
#include <render/worldRenderer/WorldRenderer.h>

namespace caustica
{

void initializeSceneSession(App& app, const SceneSessionConfig& config)
{
    GpuDevice* gpuDevice = app.getGpuDevice();
    auto* assetSystem = app.tryResource<AssetSystem>();
    auto* gpuRenderSubsystem = app.tryResource<GpuRenderSubsystem>();
    if (!gpuDevice || !assetSystem || !gpuRenderSubsystem)
        return;

    SceneViewState& viewState = config.viewState;

    sceneSession::initStreamlineAndWindow(app);
    assert(config.sessionState && "SceneSessionConfig.sessionState is required for GpuRenderSubsystem init");

    EngineSceneCallbacks sceneCallbacks{
        .OnSceneLoaded = [&app]() { sceneSession::onSceneLoaded(app); },
        .OnSceneUnloading = [&app]() { sceneSession::onSceneUnloading(app); },
    };
    if (config.hasSceneCallbacks)
        sceneCallbacks = config.sceneCallbacks;

    gpuRenderSubsystem->initializeSession(GpuRenderSubsystemInitParams{
        .gpuDevice = *gpuDevice,
        .assetSystem = *assetSystem,
        .settings = config.sessionState->settings,
        .runtimeState = config.sessionState->runtime,
        .sceneTime = viewState.sceneTime,
        .diagnostics = config.diagnostics,
        .cmdLine = config.cmdLine,
        .sceneTypeFactory = std::make_shared<render::RenderSceneTypeFactory>(),
        .sceneCallbacks = std::move(sceneCallbacks),
    });

    sceneSession::attachGpuRenderSubsystem(app, *gpuRenderSubsystem);
    sceneSession::initializeSession(app, config.preferredScene);

    if (config.refreshEnvMapMediaList)
    {
        gpuRenderSubsystem->lightingPasses().refreshEnvironmentMapMediaList(
            getLocalPath(c_AssetsFolder), std::filesystem::path());
    }

    if (config.sessionState && config.cmdLine && config.applyCmdLineToSessionState)
        render::InitializeRenderSessionStateFromCommandLine(*config.sessionState, *config.cmdLine);
}

void registerSceneSessionStartup(App& app, const SceneSessionConfig& config)
{
    app.addSystem(AppSchedule::Startup, "SceneSession.Startup", [&app, config](SystemContext& ctx) {
        (void)ctx;
        initializeSceneSession(app, config);
    });
}

void registerGpuRenderShutdown(App& app)
{
    app.addSystem(AppSchedule::shutdown, "GpuRender.shutdown", [](SystemContext& ctx) {
        if (auto* gpuRender = ctx.tryRes<GpuRenderSubsystem>())
            gpuRender->shutdown();
    });
}

} // namespace caustica

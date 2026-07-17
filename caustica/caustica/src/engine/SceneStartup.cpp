#include <engine/SceneStartup.h>

#include <scene/Scene.h>

#include <assets/AssetSystem.h>
#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/PathTracingRuntime.h>
#include <engine/RenderInfra.h>
#include <engine/SessionCamera.h>
#include <engine/SceneSession.h>
#include <engine/SceneLifecycle.h>

#include <core/path_utils.h>
#include <render/core/RenderSceneTypeFactory.h>
#include <render/RenderAppState.h>
#include <render/worldRenderer/WorldRenderer.h>

namespace caustica
{

void initializeSceneApp(App& app, const SceneAppConfig& config)
{
    GpuDevice* gpuDevice = app.getGpuDevice();
    auto* assetSystem = app.tryResource<AssetSystem>();
    auto* renderInfra = app.tryResource<RenderInfra>();
    auto* sessionCamera = app.tryResource<SessionCamera>();
    auto* sceneSession = app.tryResource<SceneSession>();
    auto* pathTracing = app.tryResource<PathTracingRuntime>();
    auto* gpuRenderSubsystem = app.tryResource<GpuRenderSubsystem>();
    if (!gpuDevice || !assetSystem || !renderInfra || !sessionCamera || !sceneSession
        || !pathTracing || !gpuRenderSubsystem)
        return;

    SceneViewState& viewState = config.viewState;

    caustica::initStreamlineAndWindow(app);
    assert(config.renderState && "SceneAppConfig.renderState is required for GpuRenderSubsystem init");

    EngineSceneCallbacks sceneCallbacks{
        .OnSceneLoaded = [&app]() { caustica::onSceneLoaded(app); },
        .OnSceneUnloading = [&app]() { caustica::onSceneUnloading(app); },
    };
    if (config.hasSceneCallbacks)
        sceneCallbacks = config.sceneCallbacks;

    gpuRenderSubsystem->initialize(GpuRenderSubsystemInitParams{
        .gpuDevice = *gpuDevice,
        .assetSystem = *assetSystem,
        .renderInfra = *renderInfra,
        .sessionCamera = *sessionCamera,
        .sceneSession = *sceneSession,
        .pathTracingRuntime = *pathTracing,
        .settings = config.renderState->settings,
        .runtimeState = config.renderState->runtime,
        .sceneTime = viewState.sceneTime,
        .diagnostics = config.diagnostics,
        .cmdLine = config.cmdLine,
        .sceneTypeFactory = std::make_shared<render::RenderSceneTypeFactory>(),
        .sceneCallbacks = std::move(sceneCallbacks),
    });

    caustica::bindSessionCameraSideEffects(app);
    caustica::initializeScene(app, config.preferredScene);

    if (config.refreshEnvMapMediaList)
    {
        pathTracing->lightingPasses().refreshEnvironmentMapMediaList(
            getLocalPath(c_AssetsFolder), std::filesystem::path());
    }

    if (config.renderState && config.cmdLine && config.applyCmdLineToRenderState)
        render::InitializeRenderAppStateFromCommandLine(*config.renderState, *config.cmdLine);
}

void registerSceneStartup(App& app, const SceneAppConfig& config)
{
    app.addSystem(AppSchedule::Startup, "Scene.Startup", [&app, config](SystemContext& ctx) {
        (void)ctx;
        initializeSceneApp(app, config);
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

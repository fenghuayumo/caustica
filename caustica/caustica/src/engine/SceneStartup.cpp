#include <engine/SceneStartup.h>

#include <cassert>

#include <assets/AssetSystem.h>
#include <engine/App.h>
#include <engine/internal/GpuRenderSubsystem.h>
#include <engine/GpuSharedCaches.h>
#include <render/core/CameraController.h>
#include <engine/SceneSession.h>
#include <engine/SceneLifecycle.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/SystemLabels.h>
#include <scene/SceneManager.h>

#include <core/log.h>
#include <core/path_utils.h>
#include <render/core/RenderSceneTypeFactory.h>
#include <render/RenderAppState.h>
#include <render/WorldRenderer.h>

namespace caustica
{

void initializeSceneApp(App& app, const SceneAppConfig& config)
{
    GpuDevice* gpuDevice = app.getGpuDevice();
    auto* assetSystem = app.tryResource<AssetSystem>();
    auto* gpuSharedCaches = app.tryResource<GpuSharedCaches>();
    auto* cam = app.tryResource<CameraController>();
    auto* sceneSession = app.tryResource<SceneSession>();
    auto* worldRenderer = app.tryResource<render::WorldRenderer>();
    auto* gpuRenderSubsystem = app.tryResource<GpuRenderSubsystem>();
    if (!gpuDevice || !assetSystem || !gpuSharedCaches || !cam || !sceneSession
        || !worldRenderer || !gpuRenderSubsystem)
        return;

    SceneViewState& viewState = config.viewState;

    caustica::initStreamlineAndWindow(app);
    assert(config.renderState && "SceneAppConfig.renderState is required for scene GPU bootstrap");

    EngineSceneCallbacks sceneCallbacks{
        .OnSceneLoaded = [&app]() { caustica::onSceneLoaded(app); },
        .OnSceneUnloading = [&app]() { caustica::onSceneUnloading(app); },
    };
    if (config.hasSceneCallbacks)
        sceneCallbacks = config.sceneCallbacks;

    if (!gpuSharedCaches->initialize(*gpuDevice, *assetSystem))
    {
        caustica::error("GpuSharedCaches::initialize failed");
        return;
    }

    cam->camera().setRotateSpeed(.003f);

    // Commit ActiveScene before host/editor loaded callbacks so early hooks see SSOT.
    auto onLoadedCb = std::move(sceneCallbacks.OnSceneLoaded);
    auto onUnloadingCb = std::move(sceneCallbacks.OnSceneUnloading);
    auto onLoaded = [onLoadedCb = std::move(onLoadedCb), &app]() {
        commitActiveSceneFromManager(app);
        if (onLoadedCb)
            onLoadedCb();
    };
    auto onUnloading = [onUnloadingCb = std::move(onUnloadingCb)]() {
        if (onUnloadingCb)
            onUnloadingCb();
    };

    if (!sceneSession->create(
            *gpuDevice,
            *gpuSharedCaches->shaderFactory,
            gpuSharedCaches->textureLoader,
            std::make_shared<render::RenderSceneTypeFactory>(),
            std::move(onLoaded),
            std::move(onUnloading)))
    {
        caustica::error("SceneSession::create failed");
        return;
    }

    if (sceneSession->manager)
    {
        sceneSession->manager->setLoadFailedCallback([&app, &viewState]() {
            caustica::error("Scene load failed");
            clearActiveScene(app);
            viewState.progressLoading.stop();
            viewState.loadSession.reset();
            viewState.sceneGpuSuspended.store(false, std::memory_order_release);
        });
    }

    if (!worldRenderer->create(render::WorldRenderer::createParams{
            .gpuDevice = *gpuDevice,
            .gpuSharedCaches = *gpuSharedCaches,
            .settings = config.renderState->settings,
            .runtimeState = config.renderState->runtime,
            .diagnostics = config.diagnostics,
            .sceneTime = viewState.sceneTime,
        }))
    {
        caustica::error("WorldRenderer::create failed");
        return;
    }

    if (!gpuRenderSubsystem->initialize(gpuRenderSubsystemInitParams{
            .app = app,
            .gpuDevice = *gpuDevice,
            .assetSystem = *assetSystem,
            .gpuSharedCaches = *gpuSharedCaches,
            .sceneSession = *sceneSession,
            .worldRenderer = *worldRenderer,
            .settings = config.renderState->settings,
            .runtimeState = config.renderState->runtime,
            .diagnostics = config.diagnostics,
        }))
    {
        caustica::error("GpuRenderSubsystem::initialize failed");
        return;
    }

    caustica::bindCameraControllerSideEffects(app);
    caustica::initializeScene(app, config.preferredScene);

    if (config.refreshEnvMapMediaList)
    {
        worldRenderer->lightingPasses().refreshEnvironmentMapMediaList(
            getLocalPath(c_AssetsFolder), std::filesystem::path());
    }

    if (config.renderState && config.cmdLine && config.applyCmdLineToRenderState)
        render::InitializeRenderAppStateFromCommandLine(*config.renderState, *config.cmdLine);
}

void registerSceneStartup(App& app, const SceneAppConfig& config)
{
    app.addSystem<system_label::SceneStartup>(AppSchedule::Startup, [&app, config](SystemContext& ctx) {
        (void)ctx;
        initializeSceneApp(app, config);
    });
}

void registerGpuRenderShutdown(App& app)
{
    app.addSystem<system_label::GpuRenderShutdown>(AppSchedule::shutdown, [](SystemContext& ctx) {
        if (auto* gpuRender = ctx.tryRes<GpuRenderSubsystem>())
            gpuRender->shutdown();
        // SceneSession reset drops SceneManager's scene, but ActiveScene may still
        // keep the Scene (+ TextureLoader) alive until AssetSystemShutdown.
        clearActiveScene(ctx.app);
    });
}

} // namespace caustica

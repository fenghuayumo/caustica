#include <engine/SceneSessionSubsystem.h>



#include <scene/Scene.h>



#include <engine/App.h>

#include <engine/GpuRenderSubsystem.h>

#include <engine/SceneSessionHooks.h>

#include <engine/SceneSessionSystems.h>

#include <engine/SubsystemCollection.h>



#include <core/path_utils.h>

#include <render/core/RenderSceneTypeFactory.h>

#include <render/RenderSessionState.h>

#include <render/worldRenderer/WorldRenderer.h>



namespace caustica

{



SceneSessionSubsystem::SceneSessionSubsystem(SceneSessionConfig config)

    : m_config(std::move(config))

{

}



void SceneSessionSubsystem::initialize(EngineInitContext& context)

{

    initializeSceneSession(context, m_config);

    onInitializePost(context);

}



void SceneSessionSubsystem::onInitializePost(EngineInitContext& /*context*/)

{

}



void initializeSceneSession(EngineInitContext& context, const SceneSessionConfig& config)

{

    if (!context.gpuDevice || !context.subsystems || !context.app)

        return;



    auto* gpuRenderSubsystem = context.subsystems->get<GpuRenderSubsystem>();

    if (!gpuRenderSubsystem)

        return;



    App& app = *context.app;

    SceneViewState& viewState = config.viewState;

    GpuDevice& gpuDevice = *context.gpuDevice;



    sceneSession::initStreamlineAndWindow(app);

    if (config.hooks)

        app.insertResourceRef(*config.hooks);



    assert(config.sessionState && "SceneSessionConfig.sessionState is required for GpuRenderSubsystem init");



    gpuRenderSubsystem->initializeSession(GpuRenderSubsystemInitParams{

        .gpuDevice = gpuDevice,

        .settings = config.sessionState->settings,

        .runtimeState = config.sessionState->runtime,

        .sceneTime = viewState.sceneTime,

        .diagnostics = config.diagnostics,

        .cmdLine = config.cmdLine,

        .sceneTypeFactory = std::make_shared<render::RenderSceneTypeFactory>(),

        .sceneCallbacks = EngineSceneCallbacks{

            .OnSceneLoaded = [&app]() { sceneSession::onSceneLoaded(app); },

            .OnSceneUnloading = [&app]() { sceneSession::onSceneUnloading(app); },

        },

    });



    sceneSession::attachGpuRenderSubsystem(app, *gpuRenderSubsystem);

    sceneSession::initializeSession(app, config.preferredScene);



    if (config.refreshEnvMapMediaList)

    {

        gpuRenderSubsystem->lightingPasses().refreshEnvironmentMapMediaList(

            GetLocalPath(c_AssetsFolder), std::filesystem::path());

    }



    if (config.sessionState && config.cmdLine && config.applyCmdLineToSessionState)

        render::InitializeRenderSessionStateFromCommandLine(*config.sessionState, *config.cmdLine);



    if (config.hooks && config.hooks->registerRenderPipelinePlugins)

        config.hooks->registerRenderPipelinePlugins(*gpuRenderSubsystem);

}



} // namespace caustica

